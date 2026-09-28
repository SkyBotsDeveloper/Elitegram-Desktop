/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#include "elitegram/elitegram_deleted_messages.h"

#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "storage/storage_account.h"
#include "history/history.h"
#include "history/history_item.h"
#include "data/data_media_types.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_document_media.h"
#include "core/application.h"
#include "ui/image/image.h"
#include "data/data_session.h"
#include "data/data_message_reaction_id.h"
#include "base/unixtime.h"
#include "logs.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QStorageInfo>
#include <QtGui/QImageReader>

#include <atomic>

#include <algorithm>
#include <iterator>

namespace Elitegram {
namespace {

constexpr auto kMagic = quint32(0x4547444D); // EGDM
constexpr auto kVersion = quint32(4);
constexpr auto kMaxTextLength = 4096;
constexpr auto kMaxEntityCount = 128;
constexpr auto kMaxEntityDataLength = 512;
constexpr auto kMaxPathLength = 1024;
constexpr auto kMaxMediaNameLength = 256;
constexpr auto kMaxMimeLength = 128;
constexpr auto kMaxReplyPreviewLength = 160;
constexpr auto kMaxReactions = size_t(16);
constexpr auto kFreeSpaceReserve = int64(256) * 1024 * 1024;
constexpr auto kMaxActiveCount = size_t(750);
constexpr auto kMaxDeletedCount = size_t(1500);
constexpr auto kMaxFileBytes = 8 * 1024 * 1024;
constexpr auto kActiveMaxAgeSeconds = TimeId(30 * 24 * 60 * 60);
constexpr auto kDiagnosticLimit = 64;

[[nodiscard]] const char *MediaKindName(uint8 subtype) {
	switch (subtype) {
	case 1: return "photo";
	case 2: return "video";
	case 3: return "document";
	case 4: return "voice";
	case 5: return "audio";
	case 6: return "animation";
	case 7: return "sticker";
	}
	return "unknown";
}

[[nodiscard]] int64 MediaLimitBytes(int index) {
	switch (index) {
	case 0: return int64(512) * 1024 * 1024;
	case 1: return int64(1024) * 1024 * 1024;
	case 2: return int64(2) * 1024 * 1024 * 1024;
	case 3: return int64(5) * 1024 * 1024 * 1024;
	case 4: return 0; // No application quota; free-space checks still apply.
	}
	return int64(1024) * 1024 * 1024;
}

[[nodiscard]] QString SafeExtension(QString name) {
	const auto suffix = QFileInfo(name).suffix().toLower();
	if (suffix.isEmpty() || suffix.size() > 8) {
		return u"bin"_q;
	}
	for (const auto ch : suffix) {
		if (!((ch >= QChar('a') && ch <= QChar('z'))
			|| (ch >= QChar('0') && ch <= QChar('9')))) {
			return u"bin"_q;
		}
	}
	return suffix;
}

[[nodiscard]] QString KnownExtension(const QString &name) {
	const auto result = SafeExtension(name);
	return result == u"bin"_q ? QString() : result;
}

[[nodiscard]] QString ExtensionFromMime(QString mime) {
	mime = mime.section(QChar(';'), 0, 0).trimmed().toLower();
	static const auto known = std::map<QString, QString>{
		{ u"audio/ogg"_q, u"ogg"_q },
		{ u"audio/opus"_q, u"opus"_q },
		{ u"audio/mpeg"_q, u"mp3"_q },
		{ u"audio/mp3"_q, u"mp3"_q },
		{ u"audio/mp4"_q, u"m4a"_q },
		{ u"audio/x-m4a"_q, u"m4a"_q },
		{ u"audio/aac"_q, u"aac"_q },
		{ u"audio/flac"_q, u"flac"_q },
		{ u"audio/x-flac"_q, u"flac"_q },
		{ u"audio/wav"_q, u"wav"_q },
		{ u"audio/x-wav"_q, u"wav"_q },
		{ u"video/mp4"_q, u"mp4"_q },
		{ u"video/webm"_q, u"webm"_q },
		{ u"video/x-matroska"_q, u"mkv"_q },
		{ u"video/quicktime"_q, u"mov"_q },
		{ u"image/gif"_q, u"gif"_q },
		{ u"image/webp"_q, u"webp"_q },
		{ u"image/png"_q, u"png"_q },
		{ u"image/jpeg"_q, u"jpg"_q },
		{ u"application/x-tgsticker"_q, u"tgs"_q },
		{ u"application/pdf"_q, u"pdf"_q },
		{ u"application/zip"_q, u"zip"_q },
	};
	const auto i = known.find(mime);
	return i == known.end() ? QString() : i->second;
}

[[nodiscard]] QString ExtensionFromHeader(const QByteArray &header) {
	if (header.startsWith("OggS")) return u"ogg"_q;
	if (header.startsWith("fLaC")) return u"flac"_q;
	if (header.startsWith("ID3")) return u"mp3"_q;
	if (header.startsWith("GIF87a") || header.startsWith("GIF89a")) return u"gif"_q;
	if (header.startsWith("\x89PNG\r\n\x1a\n")) return u"png"_q;
	if (header.startsWith("\xff\xd8\xff")) return u"jpg"_q;
	if (header.startsWith("%PDF-")) return u"pdf"_q;
	if (header.startsWith("PK\x03\x04")) return u"zip"_q;
	if (header.size() >= 12 && header.startsWith("RIFF")) {
		if (header.mid(8, 4) == "WEBP") return u"webp"_q;
		if (header.mid(8, 4) == "WAVE") return u"wav"_q;
	}
	if (header.size() >= 12 && header.mid(4, 4) == "ftyp") {
		const auto brand = header.mid(8, 4);
		if (brand == "M4A " || brand == "M4B ") return u"m4a"_q;
		if (brand == "qt  ") return u"mov"_q;
		if (brand == "isom" || brand == "iso2" || brand == "mp41"
			|| brand == "mp42" || brand == "avc1") return u"mp4"_q;
	}
	return {};
}

[[nodiscard]] QByteArray MediaHeader(const QString &source, const QByteArray &bytes) {
	if (!bytes.isEmpty()) return bytes.left(32);
	auto file = QFile(source);
	return file.open(QIODevice::ReadOnly) ? file.read(32) : QByteArray();
}

[[nodiscard]] QString ResolveRetainedMediaExtension(
		const QString &name,
		const QString &mime,
		const QString &attributeExtension,
		const QString &source,
		const QByteArray &bytes) {
	const auto named = KnownExtension(name);
	if (!named.isEmpty()) return named;
	const auto typed = ExtensionFromMime(mime);
	if (!typed.isEmpty()) return typed;
	if (!attributeExtension.isEmpty()) return attributeExtension;
	const auto detected = ExtensionFromHeader(MediaHeader(source, bytes));
	return detected.isEmpty() ? u"bin"_q : detected;
}

[[nodiscard]] bool SafeMediaLeaf(const QString &leaf) {
	if (!leaf.startsWith(u"m-"_q) || leaf.size() > 120) {
		return false;
	}
	for (const auto ch : leaf) {
		if (!((ch >= QChar('a') && ch <= QChar('z'))
			|| (ch >= QChar('0') && ch <= QChar('9'))
			|| ch == QChar('-') || ch == QChar('.'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] DeletedMessagesStore::Key KeyFor(
		not_null<const HistoryItem*> item) {
	// HistoryItem::topicRootId() falls back to ForumTopic::kGeneralId
	// even in ordinary user DMs. Archive user DMs in their history scope.
	return {
		.peer = item->history()->peer->id.value,
		.topic = 0,
		.message = item->id.bare,
	};
}

} // namespace

DeletedMessagesStore::DeletedMessagesStore(
		not_null<Main::Session*> session)
: _session(session)
, _enabled(session->settings().elitegramKeepDeletedMessages())
, _mediaLimitIndex(session->settings().elitegramDeletedMediaLimit())
, _writeTimer([=] { write(false); })
, _mediaRefreshTimer([=] { refreshLocalMedia(); }) {
	load();
	LOG(("ElitegramAntiDelete setting account=%1 enabled=%2 loaded=%3")
		.arg(_session->uniqueId()).arg(int(enabled())).arg(int(_records.size())));
	_session->downloaderTaskFinished() | rpl::on_next([=] {
		if (!_mediaRefreshTimer.isActive()) {
			_mediaRefreshTimer.callOnce(250); // Coalesce one download burst.
		}
	}, _lifetime);
	_session->data().photoLoadProgress(
	) | rpl::on_next([=](not_null<PhotoData*> photo) {
		const auto view = photo->activeMediaView();
		if (!enabled()) {
			return;
		}
		auto matches = std::vector<Key>();
		for (const auto &[key, record] : _records) {
			if (!record.deleted && record.mediaKind == 1
				&& record.mediaId == photo->id && !record.mediaPruned) {
				matches.push_back(key);
			}
		}
		for (const auto &key : matches) {
			const auto item = _session->data().message(
				PeerId(key.peer), MsgId(key.message));
			const auto full = view
				&& (view->image(Data::PhotoSize::Large) != nullptr);
			const auto old = _photoEventFull.find(key);
			const auto changed = (old == _photoEventFull.end()
				|| old->second != full);
			if (changed
				&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete photo_load_event account=%1 peer=%2 msg=%3 active=1 live=%4 full=%5")
					.arg(_session->uniqueId()).arg(key.peer).arg(key.message)
					.arg(int(item != nullptr)).arg(int(full)));
			}
			_photoEventFull[key] = full;
			if (item && (full || !photo->loading())) {
				if (changed && _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete photo_retry account=%1 peer=%2 msg=%3 full=%4")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message).arg(int(full)));
				}
				if (full && changed
					&& _photoBackgroundLoadAttempted.contains(key)
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete photo_background_load_done account=%1 peer=%2 msg=%3")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message));
				}
				captureLocalMedia(item, key);
			}
		}
	}, _lifetime);
	_session->data().documentLoadProgress(
	) | rpl::on_next([=](not_null<DocumentData*> document) {
		if (!enabled()) {
			return;
		}
		auto matches = std::vector<Key>();
		for (const auto &entry : _activeDocumentViews) {
			const auto &key = entry.first;
			const auto i = _records.find(key);
			if (i != _records.end() && !i->second.deleted
				&& i->second.mediaKind == 2
				&& i->second.mediaId == document->id
				&& !i->second.mediaPruned) {
				matches.push_back(key);
			}
		}
		for (const auto &key : matches) {
			const auto item = _session->data().message(
				PeerId(key.peer), MsgId(key.message));
			if (!item) {
				continue;
			}
			const auto kind = QString::fromLatin1(MediaKindName(
				_records.at(key).mediaSubtype));
			if (document->loading()) {
				const auto bucket = std::clamp(
					int(document->progress() * 4.), 0, 3);
				const auto old = _documentProgressBucket.find(key);
				if ((old == _documentProgressBucket.end()
						|| old->second != bucket)
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete media_background_progress account=%1 peer=%2 msg=%3 kind=%4 completed=0 quarter=%5")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message).arg(kind).arg(bucket));
				}
				_documentProgressBucket[key] = bucket;
				continue;
			}
			if ((document->status == FileDownloadFailed
					|| document->cancelled())
				&& _documentFailureLogged.insert(key).second
				&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete media_background_load_failed account=%1 peer=%2 msg=%3 kind=%4 reason=%5")
					.arg(_session->uniqueId()).arg(key.peer)
					.arg(key.message).arg(kind)
					.arg(document->cancelled()
						? u"cancelled"_q : u"loader_failed"_q));
			}
			const auto view = document->activeMediaView();
			const auto local = !document->filepath(true).isEmpty()
				|| (view && !view->bytes().isEmpty());
			if (local && _documentCompletionLogged.insert(key).second
				&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete media_background_load_done account=%1 peer=%2 msg=%3 kind=%4")
					.arg(_session->uniqueId()).arg(key.peer)
					.arg(key.message).arg(kind));
			}
			captureLocalMedia(item, key); // Completion is local-only.
		}
	}, _lifetime);
}

bool DeletedMessagesStore::enabled() const {
	return _enabled.current();
}

rpl::producer<bool> DeletedMessagesStore::enabledValue() const {
	return _enabled.value();
}

void DeletedMessagesStore::setEnabled(bool enabled) {
	if (this->enabled() == enabled) {
		return;
	}
	_enabled = enabled;
	if (!enabled) {
		++*_mediaGeneration; // Invalidate any in-flight copy from this session.
		_activePhotoViews.clear();
		_photoEventFull.clear();
		_photoBackgroundLoadAttempted.clear();
		_activeDocumentViews.clear();
		_documentBackgroundLoadAttempted.clear();
		_documentCompletionLogged.clear();
		_documentNotAvailableLogged.clear();
		_documentProgressBucket.clear();
		_documentFailureLogged.clear();
	}
	_session->settings().setElitegramKeepDeletedMessages(enabled);
	_session->saveSettingsDelayed();
	LOG(("ElitegramAntiDelete setting account=%1 enabled=%2")
		.arg(_session->uniqueId()).arg(int(enabled)));
}

int DeletedMessagesStore::mediaLimitIndex() const {
	return _mediaLimitIndex.current();
}

rpl::producer<int> DeletedMessagesStore::mediaLimitValue() const {
	return _mediaLimitIndex.value();
}

void DeletedMessagesStore::setMediaLimitIndex(int index) {
	if (index < 0 || index > 4 || index == mediaLimitIndex()) {
		return;
	}
	const auto previous = MediaLimitBytes(mediaLimitIndex());
	_mediaLimitIndex = index;
	_session->settings().setElitegramDeletedMediaLimit(index);
	_session->saveSettingsDelayed();
	pruneMedia();
	const auto increased = !MediaLimitBytes(index)
		|| (previous && MediaLimitBytes(index) > previous);
	if (increased && enabled()) {
		for (auto &[key, record] : _records) {
			if (!record.deleted && record.mediaPruned) {
				record.mediaPruned = false;
				scheduleWrite();
			}
		}
		refreshLocalMedia();
	}
}

void DeletedMessagesStore::clearArchive() {
	++*_mediaGeneration; // Cancel copies without cancelling the session.
	auto peers = std::set<uint64>();
	for (const auto &[key, record] : _records) {
		if (record.deleted) {
			peers.insert(key.peer);
		}
	}
	_writeTimer.cancel();
	_records.clear();
	_photoNotAvailableLogged.clear();
	_activePhotoViews.clear();
	_photoEventFull.clear();
	_photoBackgroundLoadAttempted.clear();
	_activeDocumentViews.clear();
	_documentBackgroundLoadAttempted.clear();
	_documentCompletionLogged.clear();
	_documentNotAvailableLogged.clear();
	_documentProgressBucket.clear();
	_documentFailureLogged.clear();
	_pendingDeletedKeys.clear();
	_pendingPromotion = false;
	_dirty = true;
	write(true); // Empty only this account's Elitegram metadata and owned media.
	for (const auto peer : peers) {
		_deletedChanges.fire_copy({ peer, 0, 0 });
	}
}

DeletedMessagesStore::~DeletedMessagesStore() {
	_mediaCancelled->store(true);
	++*_mediaGeneration;
	_writeTimer.cancel();
	_mediaRefreshTimer.cancel();
	if (_dirty) {
		write(true);
	}
}

bool DeletedMessagesStore::privatePeer(PeerId peer) const {
	return peer.is<UserId>()
		&& peer != _session->userPeerId();
}

bool DeletedMessagesStore::pruneNonPrivate() {
	auto removed = false;
	for (auto i = _records.begin(); i != _records.end();) {
		if (!privatePeer(PeerId(i->first.peer)) || i->first.topic != 0) {
			i = _records.erase(i);
			removed = true;
		} else {
			++i;
		}
	}
	return removed;
}

bool DeletedMessagesStore::eligible(
		not_null<const HistoryItem*> item) const {
	return eligibilityReason(item) == nullptr;
}

const char *DeletedMessagesStore::eligibilityReason(
		not_null<const HistoryItem*> item) const {
	const auto peer = item->history()->peer;
	if (!privatePeer(peer->id)
		|| !peer->isUser()
		|| peer->isSelf()) {
		return "non_private";
	}
	if (!item->isRegular()
		|| !IsServerMsgId(item->id)
		|| item->isService()
		|| item->isScheduled()) {
		return "non_regular";
	}
	if (item->isEphemeral()) {
		return "ephemeral";
	}
	if (item->forbidsSaving()
		|| !peer->allowsForwarding()) {
		return "protected";
	}
	const auto media = item->media();
	if (!media) {
		return nullptr;
	}
	if (media->ttlSeconds() || !media->allowsForward()
		|| media->storyId() || media->storyMention()) {
		return "media_restricted";
	}
	if (const auto document = media->document();
		document && document->forbidsFileSave()) {
		return "media_restricted";
	}
	// Documents cover ordinary files, video, voice, audio, GIFs and stickers;
	// TTL/protection gates above still apply to every subtype.
	// Other media families need a separate review before retention.
	return (media->photo()
		|| media->document()
		|| (media->webpage() && !item->originalText().empty()))
		? nullptr
		: "media_unsupported";
}

void DeletedMessagesStore::snapshot(
		not_null<HistoryItem*> item,
		bool allowBackgroundDownload) {
	const auto trace = [&](const char *result) {
		const auto peer = item->history()->peer;
		if (peer->isUser()
			&& (item->date() <= 0
				|| item->date() >= base::unixtime::now() - 600)
			&& _snapshotDiagnosticCount++ < kDiagnosticLimit) {
			LOG(("ElitegramAntiDelete snapshot account=%1 peer=%2 type=user self=%3 msg=%4 topic=%5 result=%6")
				.arg(_session->uniqueId()).arg(peer->id.value)
				.arg(int(peer->isSelf()))
				.arg(item->id.bare).arg(item->topicRootId().bare)
				.arg(QString::fromLatin1(result)));
		}
	};
	if (!enabled()) {
		trace("disabled");
		return; // Keep existing archive data, but capture nothing while off.
	}
	if (const auto reason = eligibilityReason(item)) {
		trace(reason);
		const auto key = KeyFor(item);
		_activePhotoViews.erase(key);
		_photoEventFull.erase(key);
		_photoBackgroundLoadAttempted.erase(key);
		_activeDocumentViews.erase(key);
		_documentBackgroundLoadAttempted.erase(key);
		_documentCompletionLogged.erase(key);
		_documentNotAvailableLogged.erase(key);
		_documentProgressBucket.erase(key);
		_documentFailureLogged.erase(key);
		if (_records.erase(key)) {
			scheduleWrite();
		}
		return;
	}
	const auto now = base::unixtime::now();
	if (item->date() <= 0 || item->date() + kActiveMaxAgeSeconds < now) {
		trace("age");
		return;
	}
	const auto key = KeyFor(item);
	const auto existing = _records.find(key);
	if (existing != _records.end() && existing->second.deleted) {
		trace("already_deleted");
		return;
	}
	const auto &text = item->originalText();
	if (text.text.size() > kMaxTextLength
		|| text.entities.size() > kMaxEntityCount) {
		trace("bounds");
		return;
	}
	auto record = Record();
	record.key = key;
	record.sender = item->from()->id.value;
	record.replyPeer = item->replyToFullId().peer.value;
	record.replyMessage = item->replyToFullId().msg.bare;
	if (record.replyMessage > 0
		&& (!record.replyPeer || record.replyPeer == key.peer)) {
		if (const auto target = _session->data().message(
				PeerId(key.peer), MsgId(record.replyMessage));
			target && !target->forbidsSaving()) {
			record.replyPreview = target->originalText().text.simplified()
				.left(kMaxReplyPreviewLength);
			if (record.replyPreview.isEmpty()) {
				if (const auto media = target->media()) {
					if (media->photo()) {
						record.replyPreview = u"Photo"_q;
					} else if (const auto document = media->document()) {
						record.replyPreview = document->filename()
						.left(kMaxReplyPreviewLength);
					}
				}
			}
		}
	}
	record.group = item->groupId().raw();
	record.date = item->date();
	record.text = text.text;
	record.entities.reserve(text.entities.size());
	for (const auto &reaction : item->reactions()) {
		if (record.reactions.size() == kMaxReactions) {
			break;
		}
		if (reaction.count > 0) {
			record.reactions.push_back({
				.emoji = reaction.id.emoji().left(16),
				.customId = reaction.id.custom(),
				.count = reaction.count,
			});
		}
	}
	for (const auto &entity : text.entities) {
		if (!entity.validForText(text.text.size())
			|| entity.type() == EntityType::Invalid
			|| uint8(entity.type()) > uint8(EntityType::FormattedDate)
			|| entity.data().size() > kMaxEntityDataLength) {
			trace("entity_bounds");
			return;
		}
		record.entities.push_back({
			.type = uint8(entity.type()),
			.offset = entity.offset(),
			.length = entity.length(),
			.data = entity.data(),
		});
	}
	if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			record.mediaKind = 1;
			record.mediaSubtype = 1;
			record.mediaId = photo->id;
			record.width = photo->width();
			record.height = photo->height();
			record.fileName = u"Photo"_q;
		} else if (const auto document = media->document()) {
			record.mediaKind = 2;
			record.mediaId = document->id;
			record.mediaSubtype = document->isVoiceMessage() ? 4
				: (document->isSong() || document->isAudioFile()) ? 5
				: document->sticker() ? 7
				: document->isAnimation() ? 6
				: document->isVideoFile() ? 2 : 3;
			record.mediaSize = std::max(int64(0), document->size);
			record.duration = std::max(int64(0), int64(document->duration()));
			record.width = document->dimensions.width();
			record.height = document->dimensions.height();
			record.fileName = document->filename().left(kMaxMediaNameLength);
			record.originalFileName = record.fileName;
			record.mimeType = document->mimeString().left(kMaxMimeLength);
			if (record.mediaSubtype == 5) {
				if (const auto song = document->song(); song && !song->title.isEmpty()) {
					record.fileName = (song->performer.isEmpty()
						? song->title
						: song->title + u" — "_q + song->performer
					).left(kMaxMediaNameLength);
				}
			}
		}
	}
	if (existing != _records.end()
		&& existing->second.mediaKind == record.mediaKind
		&& existing->second.mediaId == record.mediaId) {
		record.localMediaPath = existing->second.localMediaPath;
		record.mediaPruned = existing->second.mediaPruned;
		if (record.originalFileName.isEmpty()) {
			record.originalFileName = existing->second.originalFileName;
		}
	}
	if (existing != _records.end()
		&& existing->second.replyMessage == record.replyMessage
		&& record.replyPreview.isEmpty()) {
		record.replyPreview = existing->second.replyPreview;
	}
	if (existing != _records.end()) {
		const auto &before = existing->second;
		const auto same = (before.text == record.text
			&& before.entities == record.entities
			&& before.mediaKind == record.mediaKind
			&& before.mediaId == record.mediaId
			&& before.mediaSubtype == record.mediaSubtype
			&& before.mediaSize == record.mediaSize
			&& before.duration == record.duration
			&& before.width == record.width
			&& before.height == record.height
			&& before.fileName == record.fileName
			&& before.originalFileName == record.originalFileName
			&& before.mimeType == record.mimeType
			&& before.replyPreview == record.replyPreview
			&& before.reactions == record.reactions
			&& before.mediaPruned == record.mediaPruned
			&& before.localMediaPath == record.localMediaPath
			&& before.replyPeer == record.replyPeer
			&& before.replyMessage == record.replyMessage
			&& before.group == record.group
			&& before.sender == record.sender
			&& before.date == record.date);
		if (same) {
			if (allowBackgroundDownload && before.mediaKind) {
				captureLocalMedia(item, key, true);
			}
			trace("already_active");
			return;
		}
		record.edited = (before.text != record.text
			|| before.entities != record.entities)
			? now : before.edited;
		if (before.mediaKind != record.mediaKind
			|| before.mediaId != record.mediaId) {
			_activePhotoViews.erase(key);
			_photoEventFull.erase(key);
			_photoBackgroundLoadAttempted.erase(key);
			_activeDocumentViews.erase(key);
			_documentBackgroundLoadAttempted.erase(key);
			_documentCompletionLogged.erase(key);
			_documentNotAvailableLogged.erase(key);
			_documentProgressBucket.erase(key);
			_documentFailureLogged.erase(key);
		}
	}
	_records.insert_or_assign(key, std::move(record));
	if (_records.at(key).mediaKind) {
		captureLocalMedia(item, key, allowBackgroundDownload);
	}
	trace("stored");
	pruneActive();
	scheduleWrite();
}

void DeletedMessagesStore::promote(
		PeerId peer,
		MsgId message,
		HistoryItem *item,
		const char *source) {
	const auto traced = (_promoteDiagnosticCount++ < kDiagnosticLimit);
	const auto trace = [&](const char *result, PeerId resolved) {
		if (traced) {
			LOG(("ElitegramAntiDelete promote account=%1 peer=%2 msg=%3 itemMsg=%4 topic=%5 live=%6 source=%7 result=%8")
				.arg(_session->uniqueId()).arg(resolved.value)
				.arg(message.bare).arg(item ? item->id.bare : 0)
				.arg(item ? item->topicRootId().bare : 0)
				.arg(int(item != nullptr))
				.arg(QString::fromLatin1(source))
				.arg(QString::fromLatin1(result)));
		}
	};
	if (!enabled()) {
		trace("disabled", peer);
		return;
	}
	if (item) {
		// Incoming deletes may be the first observation after enabling the
		// setting or after an outgoing item receives its final server ID.
		snapshot(item, false); // A delete update must not start a download.
	}
	const auto key = item
		? KeyFor(item)
		: Key{ .peer = peer.value, .topic = 0, .message = message.bare };
	auto i = _records.find(key);
	if (i == _records.end() && !item) {
		// Non-channel delete updates do not include the dialog or topic.
		// Match only an unambiguous previously captured message ID.
		for (auto j = _records.begin(); j != _records.end(); ++j) {
			if (j->first.message == message.bare
				&& (!peer || j->first.peer == peer.value)) {
				if (i != _records.end()) {
					trace("ambiguous", peer);
					return;
				}
				i = j;
			}
		}
	}
	if (i == _records.end()) {
		trace("missing", peer);
		return;
	}
	if (i->second.deleted) {
		trace("already_deleted", PeerId(i->first.peer));
		return;
	}
	if (!privatePeer(PeerId(i->first.peer)) || i->first.topic != 0) {
		trace("non_private", PeerId(i->first.peer));
		_activePhotoViews.erase(i->first);
		_activeDocumentViews.erase(i->first);
		_records.erase(i);
		scheduleWrite();
		return;
	}
	if (item && !eligible(item)) {
		trace("ineligible", PeerId(i->first.peer));
		_activePhotoViews.erase(i->first);
		_activeDocumentViews.erase(i->first);
		_records.erase(i);
		scheduleWrite();
		return;
	}
	// Current peer restrictions must still permit retention; old snapshots
	// must not override a newly protected chat.
	const auto current = _session->data().peerLoaded(
		PeerId(i->first.peer));
	if (!current || !current->isUser() || current->isSelf()
		|| !current->allowsForwarding()) {
		trace("peer_restricted", PeerId(i->first.peer));
		_activePhotoViews.erase(i->first);
		_activeDocumentViews.erase(i->first);
		_records.erase(i);
		scheduleWrite();
		return;
	}
	if (item && i->second.mediaKind && i->second.mediaPruned) {
		// A speculative active copy may have yielded to quota pressure;
		// retry from the still-live local source at the deletion boundary.
		i->second.mediaPruned = false;
		captureLocalMedia(item, i->first);
	}
	if (item && i->second.mediaKind == 1 && !i->second.mediaPruned) {
		if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
			LOG(("ElitegramAntiDelete photo_predelete account=%1 peer=%2 msg=%3")
				.arg(_session->uniqueId()).arg(i->first.peer)
				.arg(i->first.message));
		}
		captureLocalMedia(item, i->first);
	}
	if (item && i->second.mediaKind == 2 && !i->second.mediaPruned) {
		captureLocalMedia(item, i->first); // No new loader at deletion.
	}
	if (i->second.mediaKind == 1
		&& ownedMediaPath(i->second.localMediaPath).isEmpty()
		&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
		LOG(("ElitegramAntiDelete photo_delete_before_ready account=%1 peer=%2 msg=%3 copyPending=%4")
			.arg(_session->uniqueId()).arg(i->first.peer)
			.arg(i->first.message)
			.arg(int(_pendingMedia.contains(i->first))));
	}
	if (i->second.mediaKind == 2
		&& ownedMediaPath(i->second.localMediaPath).isEmpty()
		&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
		const auto document = item && item->media()
			? item->media()->document() : nullptr;
		const auto state = _pendingMedia.contains(i->first) ? "copying"
			: !document ? "no_live_item"
			: document->loading() ? "loading"
			: document->status == FileDownloadFailed ? "loader_failed"
			: document->cancelled() ? "cancelled" : "not_local";
		LOG(("ElitegramAntiDelete media_delete_before_ready account=%1 peer=%2 msg=%3 kind=%4 state=%5 progress=%6 size=%7")
			.arg(_session->uniqueId()).arg(i->first.peer)
			.arg(i->first.message)
			.arg(QString::fromLatin1(MediaKindName(i->second.mediaSubtype)))
			.arg(QString::fromLatin1(state))
			.arg(document && document->loading()
				? int(document->progress() * 100.) : 0)
			.arg(i->second.mediaSize));
	}
	_activePhotoViews.erase(i->first);
	_photoEventFull.erase(i->first);
	_photoBackgroundLoadAttempted.erase(i->first);
	_activeDocumentViews.erase(i->first);
	_documentBackgroundLoadAttempted.erase(i->first);
	_documentCompletionLogged.erase(i->first);
	_documentNotAvailableLogged.erase(i->first);
	_documentProgressBucket.erase(i->first);
	_documentFailureLogged.erase(i->first);
	i->second.deleted = true;
	trace("success", PeerId(i->first.peer));
	_pendingDeletedKeys.push_back(i->first);
	_writeTimer.cancel();
	_dirty = true;
	_pendingPromotion = true;
}

void DeletedMessagesStore::flushDeletes() {
	if (_pendingPromotion) {
		write(true); // Commit before Telegram destroys the source items.
		_pendingPromotion = false;
		for (const auto &key : _pendingDeletedKeys) {
			if (_eventDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete changed account=%1 peer=%2 topic=%3 msg=%4")
					.arg(_session->uniqueId()).arg(key.peer)
					.arg(key.topic).arg(key.message));
			}
			_deletedChanges.fire_copy(key);
		}
		_pendingDeletedKeys.clear();
	}
}

rpl::producer<DeletedMessagesStore::Key>
DeletedMessagesStore::deletedChanges() const {
	return _deletedChanges.events();
}

void DeletedMessagesStore::clearForLogout() {
	_mediaCancelled->store(true);
	++*_mediaGeneration;
	_writeTimer.cancel();
	_mediaRefreshTimer.cancel();
	_records.clear();
	_photoNotAvailableLogged.clear();
	_activePhotoViews.clear();
	_photoEventFull.clear();
	_photoBackgroundLoadAttempted.clear();
	_activeDocumentViews.clear();
	_documentBackgroundLoadAttempted.clear();
	_documentCompletionLogged.clear();
	_documentNotAvailableLogged.clear();
	_documentProgressBucket.clear();
	_documentFailureLogged.clear();
	_pendingMedia.clear();
	_pendingDeletedKeys.clear();
	_dirty = true;
	_pendingPromotion = false;
	write(true);
}

std::vector<DeletedMessagesStore::Record> DeletedMessagesStore::deletedFor(
		PeerId peer,
		MsgId topic) const {
	auto result = std::vector<Record>();
	const auto trace = [&](const char *reason) {
		if (_queryDiagnosticCount++ < kDiagnosticLimit) {
			LOG(("ElitegramAntiDelete query account=%1 peer=%2 topic=%3 count=%4 result=%5")
				.arg(_session->uniqueId()).arg(peer.value)
				.arg(topic.bare).arg(int(result.size()))
				.arg(QString::fromLatin1(reason)));
		}
	};
	if (!enabled() || !privatePeer(peer) || topic) {
		trace("disabled_or_non_private");
		return result;
	}
	const auto current = _session->data().peerLoaded(peer);
	if (!current || !current->isUser() || current->isSelf()
		|| !current->allowsForwarding()) {
		trace("peer_unavailable_or_restricted");
		return result;
	}
	for (const auto &[key, record] : _records) {
		if (record.deleted
			&& key.peer == peer.value
			&& key.topic == topic.bare) {
			result.push_back(record);
			auto &visible = result.back();
			visible.localMediaPath = ownedMediaPath(record.localMediaPath);
			if (record.mediaKind && visible.localMediaPath.isEmpty()
				&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete media_missing account=%1 peer=%2 msg=%3 kind=%4")
					.arg(_session->uniqueId()).arg(key.peer)
					.arg(key.message).arg(record.mediaSubtype));
			}
		}
	}
	trace("ok");
	return result;
}

std::optional<DeletedMessagesStore::Record> DeletedMessagesStore::findDeleted(
		PeerId peer,
		MsgId topic,
		MsgId message) const {
	if (!enabled() || !privatePeer(peer) || topic) {
		return std::nullopt;
	}
	const auto current = _session->data().peerLoaded(peer);
	if (!current || !current->isUser() || current->isSelf()
		|| !current->allowsForwarding()) {
		return std::nullopt;
	}
	const auto i = _records.find({ peer.value, topic.bare, message.bare });
	if (i != _records.end() && i->second.deleted) {
		auto result = i->second;
		result.localMediaPath = ownedMediaPath(result.localMediaPath);
		return result;
	}
	return std::nullopt;
}

void DeletedMessagesStore::pruneActive() {
	const auto cutoff = base::unixtime::now() - kActiveMaxAgeSeconds;
	auto active = std::vector<std::pair<TimeId, Key>>();
	for (auto i = _records.begin(); i != _records.end();) {
		if (!i->second.deleted && i->second.date < cutoff) {
			i = _records.erase(i);
		} else {
			if (!i->second.deleted) {
				active.emplace_back(i->second.date, i->first);
			}
			++i;
		}
	}
	if (active.size() > kMaxActiveCount) {
		std::sort(active.begin(), active.end());
		for (auto n = size_t(0); n != active.size() - kMaxActiveCount; ++n) {
			_records.erase(active[n].second);
		}
	}
	for (auto i = _photoNotAvailableLogged.begin();
		i != _photoNotAvailableLogged.end();) {
		i = _records.contains(*i)
			? std::next(i) : _photoNotAvailableLogged.erase(i);
	}
	for (auto i = _activePhotoViews.begin(); i != _activePhotoViews.end();) {
		i = _records.contains(i->first) && !_records.at(i->first).deleted
			? std::next(i) : _activePhotoViews.erase(i);
	}
	for (auto i = _photoEventFull.begin(); i != _photoEventFull.end();) {
		i = _activePhotoViews.contains(i->first)
			? std::next(i) : _photoEventFull.erase(i);
	}
	for (auto i = _photoBackgroundLoadAttempted.begin();
		i != _photoBackgroundLoadAttempted.end();) {
		i = _activePhotoViews.contains(*i)
			? std::next(i) : _photoBackgroundLoadAttempted.erase(i);
	}
	for (auto i = _activeDocumentViews.begin();
		i != _activeDocumentViews.end();) {
		i = _records.contains(i->first) && !_records.at(i->first).deleted
			? std::next(i) : _activeDocumentViews.erase(i);
	}
	for (auto i = _documentBackgroundLoadAttempted.begin();
		i != _documentBackgroundLoadAttempted.end();) {
		i = _activeDocumentViews.contains(*i)
			? std::next(i) : _documentBackgroundLoadAttempted.erase(i);
	}
	for (auto i = _documentCompletionLogged.begin();
		i != _documentCompletionLogged.end();) {
		i = _activeDocumentViews.contains(*i)
			? std::next(i) : _documentCompletionLogged.erase(i);
	}
	for (auto i = _documentNotAvailableLogged.begin();
		i != _documentNotAvailableLogged.end();) {
		i = _activeDocumentViews.contains(*i)
			? std::next(i) : _documentNotAvailableLogged.erase(i);
	}
	for (auto i = _documentProgressBucket.begin();
		i != _documentProgressBucket.end();) {
		i = _activeDocumentViews.contains(i->first)
			? std::next(i) : _documentProgressBucket.erase(i);
	}
	for (auto i = _documentFailureLogged.begin();
		i != _documentFailureLogged.end();) {
		i = _activeDocumentViews.contains(*i)
			? std::next(i) : _documentFailureLogged.erase(i);
	}
}

void DeletedMessagesStore::refreshLocalMedia() {
	auto candidates = std::vector<Key>();
	for (const auto &[key, record] : _records) {
		if (!record.deleted
			&& record.mediaKind
			&& !record.mediaPruned
			&& (record.localMediaPath.isEmpty()
				|| ownedMediaPath(record.localMediaPath).isEmpty())) {
			candidates.push_back(key);
		}
	}
	for (const auto &key : candidates) {
		if (const auto item = _session->data().message(
				PeerId(key.peer), MsgId(key.message))) {
			snapshot(item);
			captureLocalMedia(item, key);
		}
	}
}

QString DeletedMessagesStore::ownedMediaPath(const QString &leaf) const {
	if (!SafeMediaLeaf(leaf)) {
		return {};
	}
	const auto folder = _session->local().elitegramDeletedMediaPath();
	if (QFileInfo(folder).isSymLink()) {
		return {};
	}
	const auto path = folder + leaf;
	const auto info = QFileInfo(path);
	const auto canonicalRoot = QDir(folder).canonicalPath();
	const auto root = canonicalRoot + QChar('/');
	return (info.isFile() && !info.isSymLink()
		&& !canonicalRoot.isEmpty()
		&& info.canonicalFilePath().startsWith(root, Qt::CaseInsensitive))
		? path : QString();
}

void DeletedMessagesStore::captureLocalMedia(
		not_null<HistoryItem*> item,
		const Key &key,
		bool allowBackgroundDownload) {
	if (!enabled() || !eligible(item) || !privatePeer(PeerId(key.peer))) {
		return;
	}
	const auto i = _records.find(key);
	if (i == _records.end() || !i->second.mediaKind
		|| !i->second.mediaId || i->second.mediaPruned) {
		return;
	}
	if (ownedMediaPath(i->second.localMediaPath).size()) {
		return;
	}
	if (!i->second.localMediaPath.isEmpty()) {
		i->second.localMediaPath.clear();
		scheduleWrite();
	}
	if (_pendingMedia.contains(key)) {
		return;
	}
	auto source = QString();
	auto bytes = QByteArray();
	auto extension = QString();
	auto preview = QImage();
	if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			if (photo->uploading()) {
				return;
			}
			if (!_activePhotoViews.contains(key)) {
				_activePhotoViews.emplace(key, photo->createMediaView());
				if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete photo_active account=%1 peer=%2 msg=%3")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message));
				}
			}
			auto hadView = false;
			auto hadFullImage = false;
			if (!photo->loading()) {
				source = photo->location(true).name();
				if (!QFileInfo(source).isFile()
					|| QFileInfo(source).isSymLink()) {
					source.clear();
				}
			}
			if (const auto view = photo->activeMediaView()) {
				hadView = true;
				hadFullImage = (view->image(Data::PhotoSize::Large) != nullptr);
				if (source.isEmpty()) {
					bytes = view->imageBytes(Data::PhotoSize::Large);
					if (bytes.isEmpty()) {
						if (const auto image = view->image(Data::PhotoSize::Large)) {
							preview = image->original(); // Full image only.
						}
					}
				}
			}
			extension = !source.isEmpty() ? SafeExtension(source)
				: !bytes.isEmpty() ? u"jpg"_q : u"png"_q;
			if (source.isEmpty() && bytes.isEmpty() && preview.isNull()) {
				// Only an active message snapshot may start Telegram's normal
				// loader. Delete-time capture and deleted rows never do so.
				if (allowBackgroundDownload
					&& !photo->failed(Data::PhotoSize::Large)
					&& _photoBackgroundLoadAttempted.insert(key).second) {
					if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
						LOG(("ElitegramAntiDelete photo_background_candidate account=%1 peer=%2 msg=%3 photo=%4")
							.arg(_session->uniqueId()).arg(key.peer)
							.arg(key.message).arg(i->second.mediaId));
					}
					if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
						LOG(("ElitegramAntiDelete photo_background_load_start account=%1 peer=%2 msg=%3")
							.arg(_session->uniqueId()).arg(key.peer)
							.arg(key.message));
					}
					photo->load(Data::PhotoSize::Large,
						Data::FileOrigin(item->fullId()),
						LoadFromCloudOrLocal,
						true);
				}
				if (_photoNotAvailableLogged.insert(key).second
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete photo_source account=%1 peer=%2 msg=%3 source=not_available view=%4 full=%5")
						.arg(_session->uniqueId()).arg(key.peer).arg(key.message)
						.arg(int(hadView)).arg(int(hadFullImage)));
				}
				return;
			}
			_photoNotAvailableLogged.erase(key);
			if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete photo_source account=%1 peer=%2 msg=%3 source=%4")
					.arg(_session->uniqueId()).arg(key.peer).arg(key.message)
					.arg(source.isEmpty() ? u"loaded_image"_q : u"file"_q));
			}
		} else if (const auto document = media->document()) {
			if (document->uploading()) {
				return;
			}
			if (!_activeDocumentViews.contains(key)) {
				_activeDocumentViews.emplace(
					key, document->createMediaView());
			}
			if (!document->loading()) {
				source = document->filepath(true);
				if (!QFileInfo(source).isFile()
					|| QFileInfo(source).isSymLink()) {
					source.clear();
				}
				if (const auto view = document->activeMediaView()) {
					if (source.isEmpty()) {
						bytes = view->bytes();
					}
					if (const auto thumbnail = view->thumbnail()) {
						preview = thumbnail->original();
					}
				}
			}
			const auto sticker = document->sticker();
			const auto stickerExtension = sticker
				? sticker->isLottie() ? u"tgs"_q
					: sticker->isWebm() ? u"webm"_q : u"webp"_q
				: QString();
			extension = ResolveRetainedMediaExtension(
				document->filename(), document->mimeString(),
				stickerExtension, source, bytes);
			if (source.isEmpty() && bytes.isEmpty()) {
				if (allowBackgroundDownload
					&& !_documentBackgroundLoadAttempted.contains(key)) {
					const auto limit = MediaLimitBytes(mediaLimitIndex());
					const auto plausibleSize = std::max(int64(0), document->size);
					const auto archiveFree = QStorageInfo(
						QFileInfo(_session->local().elitegramDeletedMediaPath()
						).absolutePath()
					).bytesAvailable();
					if ((!limit || plausibleSize <= limit)
						&& (!plausibleSize || (archiveFree > plausibleSize
							&& archiveFree - plausibleSize >= kFreeSpaceReserve))
						&& _documentBackgroundLoadAttempted.insert(key).second) {
						const auto toCache = document->saveToCache();
						const auto alreadyLoading = document->loading();
						const auto canSave = alreadyLoading || toCache
							|| Core::App().canSaveFileWithoutAskingForPath();
						const auto toFile = alreadyLoading
							? document->loadingFilePath()
							: toCache ? QString()
							: canSave ? DocumentFileNameForSave(document)
							: QString();
						if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
							LOG(("ElitegramAntiDelete media_background_strategy account=%1 peer=%2 msg=%3 kind=%4 strategy=%5 bytes=%6")
								.arg(_session->uniqueId()).arg(key.peer)
								.arg(key.message)
								.arg(QString::fromLatin1(MediaKindName(
									i->second.mediaSubtype)))
								.arg(document->loading() ? u"existing_loader"_q
									: toCache ? u"telegram_cache"_q
									: canSave ? u"telegram_file"_q
									: u"path_required"_q)
								.arg(plausibleSize));
						}
						if (canSave && (alreadyLoading || toCache
							|| !toFile.isEmpty())) {
							if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
								LOG(("ElitegramAntiDelete media_background_candidate account=%1 peer=%2 msg=%3 kind=%4")
									.arg(_session->uniqueId()).arg(key.peer)
									.arg(key.message)
									.arg(QString::fromLatin1(MediaKindName(
										i->second.mediaSubtype))));
							}
							if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
								LOG(("ElitegramAntiDelete media_background_load_start account=%1 peer=%2 msg=%3 kind=%4")
									.arg(_session->uniqueId()).arg(key.peer)
									.arg(key.message)
									.arg(QString::fromLatin1(MediaKindName(
										i->second.mediaSubtype))));
							}
							if (alreadyLoading) {
								document->permitLoadFromCloud();
							} else {
								document->save(
									Data::FileOrigin(item->fullId()),
									toFile,
									LoadFromCloudOrLocal,
									true);
							}
							if (!document->loading()
								&& document->filepath(true).isEmpty()
								&& (!document->activeMediaView()
									|| document->activeMediaView()->bytes().isEmpty())
								&& _documentFailureLogged.insert(key).second
								&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
								LOG(("ElitegramAntiDelete media_background_load_failed account=%1 peer=%2 msg=%3 kind=%4 reason=did_not_start")
									.arg(_session->uniqueId()).arg(key.peer)
									.arg(key.message)
									.arg(QString::fromLatin1(MediaKindName(
										i->second.mediaSubtype))));
							}
						} else if (_documentFailureLogged.insert(key).second
							&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
							LOG(("ElitegramAntiDelete media_background_load_failed account=%1 peer=%2 msg=%3 kind=%4 reason=%5")
								.arg(_session->uniqueId()).arg(key.peer)
								.arg(key.message)
								.arg(QString::fromLatin1(MediaKindName(
									i->second.mediaSubtype)))
								.arg(canSave ? u"no_filename"_q
									: u"path_required"_q));
						}
					}
				}
				if (_documentNotAvailableLogged.insert(key).second
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete media_source account=%1 peer=%2 msg=%3 kind=%4 source=not_available")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message)
						.arg(QString::fromLatin1(MediaKindName(
							i->second.mediaSubtype))));
				}
				return;
			}
			_documentNotAvailableLogged.erase(key);
			if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete media_source account=%1 peer=%2 msg=%3 kind=%4 source=%5")
					.arg(_session->uniqueId()).arg(key.peer)
					.arg(key.message)
					.arg(QString::fromLatin1(MediaKindName(
						i->second.mediaSubtype)))
					.arg(source.isEmpty() ? u"loaded"_q : u"file"_q));
			}
		}
	}
	if (source.isEmpty() && bytes.isEmpty()
		&& (i->second.mediaSubtype != 1 || preview.isNull())) {
		if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
			LOG(("ElitegramAntiDelete media_copy_skip account=%1 peer=%2 msg=%3 kind=%4 result=not_local")
				.arg(_session->uniqueId()).arg(key.peer)
				.arg(key.message).arg(i->second.mediaSubtype));
		}
		return;
	}
	if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
		LOG(("ElitegramAntiDelete media_candidate account=%1 peer=%2 msg=%3 kind=%4 bytes=%5")
			.arg(_session->uniqueId()).arg(key.peer).arg(key.message)
			.arg(i->second.mediaSubtype).arg(int(bytes.size())));
	}
	queueMediaCopy(key, i->second.mediaId, i->second.mediaSubtype,
		std::move(source), std::move(bytes), std::move(extension),
		std::move(preview));
}

void DeletedMessagesStore::queueMediaCopy(
		const Key &key,
		uint64 mediaId,
		uint8 mediaSubtype,
		QString source,
		QByteArray bytes,
		QString extension,
		QImage preview) {
	if (!_pendingMedia.insert(key).second) {
		return;
	}
	const auto identity = u"%1:%2:%3"_q.arg(key.peer)
		.arg(key.message).arg(mediaId).toUtf8();
	const auto digest = QCryptographicHash::hash(
		identity, QCryptographicHash::Sha256).toHex().left(32);
	const auto leaf = u"m-%1.%2"_q
		.arg(QString::fromLatin1(digest))
		.arg(SafeExtension(u"x."_q + extension));
	const auto folder = _session->local().elitegramDeletedMediaPath();
	const auto cancelled = _mediaCancelled;
	const auto generation = _mediaGeneration;
	const auto startedAt = generation->load();
	const auto limit = MediaLimitBytes(mediaLimitIndex());
	const auto weak = base::make_weak(this);
	crl::async([=, source = std::move(source), bytes = std::move(bytes),
		preview = std::move(preview)]() mutable {
		auto copied = false;
		auto wroteNew = false;
		auto size = int64(0);
		auto result = "unavailable";
		const auto destination = folder + leaf;
		if (mediaSubtype == 1 && source.isEmpty() && bytes.isEmpty()
			&& !preview.isNull() && !cancelled->load()
			&& generation->load() == startedAt) {
			auto encoded = QBuffer(&bytes);
			if (!encoded.open(QIODevice::WriteOnly)
				|| !preview.save(&encoded, "PNG")) {
				bytes.clear();
			}
		}
		if (!cancelled->load() && generation->load() == startedAt
			&& SafeMediaLeaf(leaf)
			&& QDir().mkpath(folder)
			&& !QFileInfo(folder).isSymLink()) {
			const auto old = QFileInfo(destination);
			if (old.isFile() && !old.isSymLink() && old.size() > 0) {
				copied = true;
				size = old.size();
				result = "already_owned";
			} else {
				const auto inputInfo = QFileInfo(source);
				const auto useFile = inputInfo.isFile() && !inputInfo.isSymLink();
				const auto candidateSize = useFile
					? inputInfo.size() : int64(bytes.size());
				const auto available = QStorageInfo(folder).bytesAvailable();
				result = candidateSize <= 0 ? "not_local"
					: (limit && candidateSize > limit) ? "quota"
					: available <= candidateSize
						|| available - candidateSize < kFreeSpaceReserve
						? "disk_space" : "io_failed";
				if (candidateSize > 0
					&& (!limit || candidateSize <= limit)
					&& available > candidateSize
					&& available - candidateSize >= kFreeSpaceReserve) {
					auto input = QFile(source);
					auto output = QSaveFile(destination);
					if ((!useFile || input.open(QIODevice::ReadOnly))
						&& output.open(QIODevice::WriteOnly)) {
						copied = true;
						if (useFile) {
							while (!input.atEnd()
								&& !cancelled->load()
								&& generation->load() == startedAt) {
								const auto chunk = input.read(256 * 1024);
								if (chunk.isEmpty()
									|| output.write(chunk) != chunk.size()) {
									copied = false;
									break;
								}
							}
						} else {
							for (auto offset = qsizetype(); offset < bytes.size();
								offset += 256 * 1024) {
								const auto length = std::min(
									qsizetype(256 * 1024), bytes.size() - offset);
								if (cancelled->load()
									|| generation->load() != startedAt
									|| output.write(bytes.constData() + offset,
										length) != length) {
									copied = false;
									break;
								}
							}
						}
						if (copied && !cancelled->load()
							&& generation->load() == startedAt) {
							copied = output.commit();
							wroteNew = copied;
						} else {
							output.cancelWriting();
							copied = false;
						}
						if (copied) {
							size = QFileInfo(destination).size();
							copied = (size > 0 && size == candidateSize);
							if (copied) {
								result = "ok";
							}
						}
					}
				}
			}
		}
		if (copied && !cancelled->load()
			&& generation->load() == startedAt
			&& (!preview.isNull()
				|| mediaSubtype == 1 || mediaSubtype == 7)) {
			const auto previewPath = destination + u".thumb.png"_q;
			if (!QFileInfo(previewPath).isFile()) {
				auto image = preview;
				if (image.isNull()) {
					auto reader = QImageReader(destination);
					if (const auto original = reader.size(); original.isValid()
						&& int64(original.width()) * original.height() <= 40000000) {
						reader.setScaledSize(original.scaled(
							QSize(320, 240), Qt::KeepAspectRatio));
						image = reader.read();
					}
				}
				if (!image.isNull()) {
					image = image.scaled(
						QSize(320, 240), Qt::KeepAspectRatio,
						Qt::SmoothTransformation);
					auto preview = QSaveFile(previewPath);
					if (preview.open(QIODevice::WriteOnly)
						&& image.save(&preview, "PNG")) {
						preview.commit();
					}
				}
			}
		}
		if (wroteNew && (cancelled->load()
			|| generation->load() != startedAt)) {
			QFile::remove(destination);
			QFile::remove(destination + u".thumb.png"_q);
			copied = false;
			result = "cancelled";
		}
		crl::on_main(weak, [=] {
			_pendingMedia.erase(key);
			const auto i = _records.find(key);
			if (copied && !cancelled->load()
				&& generation->load() == startedAt && enabled()
				&& i != _records.end() && i->second.mediaId == mediaId) {
				i->second.localMediaPath = leaf;
				i->second.mediaSize = size;
				if (mediaSubtype == 1) {
					_activePhotoViews.erase(key);
					_photoEventFull.erase(key);
					_photoBackgroundLoadAttempted.erase(key);
				} else {
					_activeDocumentViews.erase(key);
					_documentBackgroundLoadAttempted.erase(key);
					_documentCompletionLogged.erase(key);
					_documentNotAvailableLogged.erase(key);
					_documentProgressBucket.erase(key);
					_documentFailureLogged.erase(key);
				}
				if (mediaSubtype == 1
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete photo_retain_done account=%1 peer=%2 msg=%3 bytes=%4")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message).arg(size));
				}
				if (mediaSubtype != 1
					&& _mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete media_retain_done account=%1 peer=%2 msg=%3 kind=%4 bytes=%5")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message)
						.arg(QString::fromLatin1(MediaKindName(mediaSubtype)))
						.arg(size));
				}
				if (i->second.deleted) {
					scheduleWrite();
					write(true);
					_deletedChanges.fire_copy(key);
				} else {
					scheduleWrite();
				}
			}
			if (i != _records.end() && !i->second.deleted
				&& i->second.mediaId != mediaId) {
				if (const auto item = _session->data().message(
						PeerId(key.peer), MsgId(key.message))) {
					captureLocalMedia(item, key);
				}
			}
			if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
				LOG(("ElitegramAntiDelete media_copy_done account=%1 peer=%2 msg=%3 result=%4 size=%5")
					.arg(_session->uniqueId()).arg(key.peer).arg(key.message)
					.arg(QString::fromLatin1(result)).arg(size));
			}
			if (_pendingMedia.empty()) {
				pruneMedia();
			}
		});
	});
}

void DeletedMessagesStore::pruneMedia() {
	if (!_pendingMedia.empty()) {
		return;
	}
	const auto folder = _session->local().elitegramDeletedMediaPath();
	if (QFileInfo(folder).isSymLink()) {
		return;
	}
	const auto dir = QDir(folder);
	auto referenced = std::set<QString>();
	auto candidates = std::vector<std::pair<std::pair<bool, TimeId>, Key>>();
	auto total = int64(0);
	for (auto &[key, record] : _records) {
		if (const auto path = ownedMediaPath(record.localMediaPath);
			!path.isEmpty()) {
			referenced.insert(record.localMediaPath);
			total += QFileInfo(path).size();
			const auto previewLeaf = record.localMediaPath + u".thumb.png"_q;
			const auto preview = ownedMediaPath(previewLeaf);
			if (!preview.isEmpty()) {
				referenced.insert(previewLeaf);
				total += QFileInfo(preview).size();
			}
			// Speculative active copies yield before already-deleted history.
			candidates.push_back({ { record.deleted, record.date }, key });
		} else if (!record.localMediaPath.isEmpty()) {
			record.localMediaPath.clear();
			scheduleWrite();
		}
	}
	std::sort(candidates.begin(), candidates.end());
	const auto limit = MediaLimitBytes(mediaLimitIndex());
	for (const auto &[order, key] : candidates) {
		if (!limit || total <= limit) {
			break;
		}
		auto &record = _records.at(key);
		const auto leaf = record.localMediaPath;
		const auto path = ownedMediaPath(leaf);
		if (!path.isEmpty()) {
			const auto size = QFileInfo(path).size();
			if (QFile::remove(path)) {
				total -= size;
				referenced.erase(leaf);
				const auto previewLeaf = leaf + u".thumb.png"_q;
				const auto preview = ownedMediaPath(previewLeaf);
				if (!preview.isEmpty()) {
					total -= QFileInfo(preview).size();
					QFile::remove(preview);
					referenced.erase(previewLeaf);
				}
				record.localMediaPath.clear();
				record.mediaPruned = true;
				scheduleWrite();
				if (record.deleted) {
					_deletedChanges.fire_copy(key);
				}
				if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
					LOG(("ElitegramAntiDelete media_prune account=%1 peer=%2 msg=%3 bytes=%4")
						.arg(_session->uniqueId()).arg(key.peer)
						.arg(key.message).arg(size));
				}
			}
		}
	}
	for (const auto &info : dir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
		const auto leaf = info.fileName();
		if (SafeMediaLeaf(leaf) && !referenced.contains(leaf)) {
			QFile::remove(info.absoluteFilePath());
		}
	}
}

void DeletedMessagesStore::scheduleWrite() {
	_dirty = true;
	_writeTimer.callOnce(2000);
}

void DeletedMessagesStore::write(bool sync) {
	if (!_dirty) {
		return;
	}
	(void)pruneNonPrivate();
	const auto serialize = [&] {
		auto bytes = QByteArray();
		QDataStream stream(&bytes, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << kMagic << kVersion << quint64(_session->uniqueId())
			<< quint32(_records.size());
		for (const auto &[key, record] : _records) {
			stream << quint64(key.peer) << qint64(key.topic)
				<< qint64(key.message) << quint64(record.sender)
				<< quint64(record.replyPeer) << qint64(record.replyMessage)
				<< quint64(record.group) << quint64(record.mediaId)
				<< quint8(record.mediaKind) << qint32(record.date)
				<< qint32(record.edited) << quint8(record.deleted)
				<< record.text << record.localMediaPath
				<< quint32(record.entities.size());
			for (const auto &entity : record.entities) {
				stream << quint8(entity.type) << qint32(entity.offset)
					<< qint32(entity.length) << entity.data;
			}
			stream << quint8(record.mediaSubtype)
				<< qint64(record.mediaSize) << qint64(record.duration)
				<< qint32(record.width) << qint32(record.height)
				<< record.fileName << record.mimeType
				<< quint8(record.mediaPruned);
			stream << record.replyPreview << quint32(record.reactions.size());
			for (const auto &reaction : record.reactions) {
				stream << reaction.emoji << quint64(reaction.customId)
					<< qint32(reaction.count);
			}
			stream << record.originalFileName;
		}
		return bytes;
	};
	// Active-cache cleanup never removes deleted archives. A separate archive
	// quota applies only after its explicit 1500-record limit is reached.
	auto deleted = std::vector<std::pair<TimeId, Key>>();
	for (const auto &[key, record] : _records) {
		if (record.deleted) {
			deleted.emplace_back(record.date, key);
		}
	}
	std::sort(deleted.begin(), deleted.end());
	while (deleted.size() > kMaxDeletedCount) {
		_records.erase(deleted.front().second);
		deleted.erase(deleted.begin());
	}
	auto bytes = serialize();
	while (bytes.size() > kMaxFileBytes && !_records.empty()) {
		const auto oldest = std::min_element(
			_records.begin(), _records.end(), [](const auto &a, const auto &b) {
				return std::pair(a.second.deleted, a.second.date)
					< std::pair(b.second.deleted, b.second.date);
			});
		_records.erase(oldest);
		bytes = serialize();
	}
	_session->local().writeElitegramDeletedMessages(bytes, sync);
	if (_persistDiagnosticCount++ < 16) {
		LOG(("ElitegramAntiDelete persist account=%1 records=%2 bytes=%3 sync=%4")
			.arg(_session->uniqueId()).arg(int(_records.size()))
			.arg(int(bytes.size())).arg(int(sync)));
	}
	_dirty = false;
	pruneMedia();
}

void DeletedMessagesStore::load() {
	const auto bytes = _session->local().readElitegramDeletedMessages();
	if (bytes.isEmpty() || bytes.size() > kMaxFileBytes) {
		LOG(("ElitegramAntiDelete load account=%1 bytes=%2 result=%3")
			.arg(_session->uniqueId()).arg(int(bytes.size()))
			.arg(bytes.isEmpty() ? u"empty"_q : u"oversize"_q));
		return;
	}
	QBuffer buffer;
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly)) {
		LOG(("ElitegramAntiDelete load account=%1 result=buffer_open_failed")
			.arg(_session->uniqueId()));
		return;
	}
	QDataStream stream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto version = quint32();
	auto owner = quint64();
	auto count = quint32();
	stream >> magic >> version >> owner >> count;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| (version != 1 && version != 2 && version != 3 && version != kVersion)
		|| owner != _session->uniqueId()
		|| count > kMaxActiveCount + kMaxDeletedCount) {
		LOG(("ElitegramAntiDelete load account=%1 result=invalid_header")
			.arg(_session->uniqueId()));
		return;
	}
	auto records = std::map<Key, Record>();
	for (auto n = quint32(); n != count; ++n) {
		auto record = Record();
		auto peer = quint64();
		auto topic = qint64();
		auto message = qint64();
		auto sender = quint64();
		auto replyPeer = quint64();
		auto replyMessage = qint64();
		auto group = quint64();
		auto mediaId = quint64();
		auto mediaKind = quint8();
		auto date = qint32();
		auto edited = qint32();
		auto deleted = quint8();
		auto entities = quint32();
		stream >> peer >> topic >> message >> sender >> replyPeer
			>> replyMessage >> group >> mediaId >> mediaKind >> date
			>> edited >> deleted >> record.text >> record.localMediaPath
			>> entities;
		if (stream.status() != QDataStream::Ok
			|| !peer || message <= 0 || message >= ServerMaxMsgId.bare
			|| date <= 0 || deleted > 1 || mediaKind > 2
			|| record.text.size() > kMaxTextLength
			|| record.localMediaPath.size() > kMaxPathLength
			|| entities > kMaxEntityCount) {
			LOG(("ElitegramAntiDelete load account=%1 result=invalid_record index=%2")
				.arg(_session->uniqueId()).arg(n));
			return;
		}
		record.key = { peer, topic, message };
		record.sender = sender;
		record.replyPeer = replyPeer;
		record.replyMessage = replyMessage;
		record.group = group;
		record.mediaId = mediaId;
		record.mediaKind = mediaKind;
		record.date = date;
		record.edited = edited;
		record.deleted = deleted;
		for (auto e = quint32(); e != entities; ++e) {
			auto type = quint8();
			auto offset = qint32();
			auto length = qint32();
			auto data = QString();
			stream >> type >> offset >> length >> data;
			if (stream.status() != QDataStream::Ok
				|| type == 0 || type > uint8(EntityType::FormattedDate)
				|| data.size() > kMaxEntityDataLength
				|| offset < 0 || length <= 0
				|| offset > record.text.size()
				|| length > record.text.size() - offset) {
				LOG(("ElitegramAntiDelete load account=%1 result=invalid_entity index=%2 entity=%3")
					.arg(_session->uniqueId()).arg(n).arg(e));
				return;
			}
			record.entities.push_back({ type, offset, length, data });
		}
		if (version == 1) {
			// V1 stored a Telegram cache/download path, not owned archive bytes.
			record.localMediaPath.clear();
			record.mediaSubtype = record.mediaKind == 1 ? 1
				: record.mediaKind == 2 ? 3 : 0;
		} else {
			auto mediaPruned = quint8();
			stream >> record.mediaSubtype >> record.mediaSize
				>> record.duration >> record.width >> record.height
				>> record.fileName >> record.mimeType >> mediaPruned;
			record.mediaPruned = (mediaPruned == 1);
			if (stream.status() != QDataStream::Ok
				|| mediaPruned > 1
				|| record.mediaSubtype > 7
				|| record.mediaSize < 0
				|| record.duration < 0
				|| record.width < 0 || record.height < 0
				|| record.fileName.size() > kMaxMediaNameLength
				|| record.mimeType.size() > kMaxMimeLength) {
				LOG(("ElitegramAntiDelete load account=%1 result=invalid_media index=%2")
					.arg(_session->uniqueId()).arg(n));
				return;
			}
			if (!record.localMediaPath.isEmpty()
				&& !SafeMediaLeaf(record.localMediaPath)) {
				record.localMediaPath.clear();
			}
			if (version >= 3) {
				auto count = quint32();
				stream >> record.replyPreview >> count;
				if (stream.status() != QDataStream::Ok
					|| record.replyPreview.size() > kMaxReplyPreviewLength
					|| count > kMaxReactions) {
					return;
				}
				for (auto r = quint32(); r != count; ++r) {
					auto emoji = QString();
					auto customId = quint64();
					auto reactionCount = qint32();
					stream >> emoji >> customId >> reactionCount;
					if (stream.status() != QDataStream::Ok
						|| emoji.size() > 16 || reactionCount <= 0) {
						return;
					}
					record.reactions.push_back({ emoji, customId, reactionCount });
				}
			}
			if (version >= 4) {
				stream >> record.originalFileName;
				if (stream.status() != QDataStream::Ok
					|| record.originalFileName.size() > kMaxMediaNameLength) {
					return;
				}
			} else {
				record.originalFileName = record.fileName;
			}
		}
		const auto key = record.key;
		if (!records.emplace(key, std::move(record)).second) {
			LOG(("ElitegramAntiDelete load account=%1 result=duplicate index=%2")
				.arg(_session->uniqueId()).arg(n));
			return;
		}
	}
	if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
		LOG(("ElitegramAntiDelete load account=%1 result=invalid_tail")
			.arg(_session->uniqueId()));
		return;
	}
	_records = std::move(records);
	LOG(("ElitegramAntiDelete load account=%1 result=ok records=%2")
		.arg(_session->uniqueId()).arg(int(_records.size())));
	// Repair only owned legacy .bin files whose recorded name or MIME proves
	// the container. Unknown types and collisions remain untouched.
	const auto folder = _session->local().elitegramDeletedMediaPath();
	for (auto &[key, record] : _records) {
		if (record.mediaKind != 2
			|| !record.localMediaPath.endsWith(u".bin"_q)) {
			continue;
		}
		// V2/V3 music stored a display title in fileName, not the original
		// Telegram filename; a dotted title is not reliable file metadata.
		const auto named = (version < 4 && record.mediaSubtype == 5)
			? QString() : KnownExtension(record.originalFileName);
		const auto typed = ExtensionFromMime(record.mimeType);
		const auto extension = !named.isEmpty() ? named : typed;
		if (extension.isEmpty()) {
			continue;
		}
		const auto previous = record.localMediaPath;
		const auto repaired = previous.left(previous.size() - 3) + extension;
		if (!SafeMediaLeaf(repaired)) {
			continue;
		}
		const auto oldPath = ownedMediaPath(previous);
		const auto newInfo = QFileInfo(folder + repaired);
		if (!oldPath.isEmpty() && !newInfo.exists()
			&& !newInfo.isSymLink()) {
			if (!QFile::rename(oldPath, folder + repaired)) {
				continue;
			}
			const auto oldPreview = ownedMediaPath(previous + u".thumb.png"_q);
			const auto newPreview = folder + repaired + u".thumb.png"_q;
			if (!oldPreview.isEmpty()
				&& !QFileInfo(newPreview).exists()
				&& !QFileInfo(newPreview).isSymLink()) {
				QFile::rename(oldPreview, newPreview);
			}
		} else if (!oldPath.isEmpty() || ownedMediaPath(repaired).isEmpty()) {
			continue;
		}
		record.localMediaPath = repaired;
		scheduleWrite();
		if (_mediaDiagnosticCount++ < kDiagnosticLimit) {
			LOG(("ElitegramAntiDelete media_format_repair account=%1 kind=%2 result=updated")
				.arg(_session->uniqueId()).arg(record.mediaSubtype));
		}
	}
	if (_dirty) {
		write(true); // Persist a completed rename before orphan pruning.
	}
	if (pruneNonPrivate()) {
		scheduleWrite(); // Rewrite only this account's archive, preserving DMs.
	}
	if (version < kVersion) {
		scheduleWrite(); // Preserve V1/V2 records in the current format.
	}
	pruneActive();
	pruneMedia();
}

} // namespace Elitegram
