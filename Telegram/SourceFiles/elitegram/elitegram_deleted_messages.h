/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "rpl/lifetime.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <compare>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class HistoryItem;
namespace Data {
class PhotoMedia;
class DocumentMedia;
}

namespace Main {
class Session;
} // namespace Main

namespace Elitegram {

class DeletedMessagesStore final : public base::has_weak_ptr {
public:
	struct Key {
		uint64 peer = 0;
		int64 topic = 0;
		int64 message = 0;
		friend auto operator<=>(const Key &, const Key &) = default;
	};
	struct Entity {
		uint8 type = 0;
		int32 offset = 0;
		int32 length = 0;
		QString data;
		friend bool operator==(const Entity &, const Entity &) = default;
	};
	struct Reaction {
		QString emoji;
		uint64 customId = 0;
		int32 count = 0;
		friend bool operator==(const Reaction &, const Reaction &) = default;
	};
	struct Record {
		Key key;
		uint64 sender = 0;
		uint64 replyPeer = 0;
		int64 replyMessage = 0;
		uint64 group = 0;
		uint64 mediaId = 0;
		uint8 mediaKind = 0; // 0: none, 1: photo, 2: document.
		uint8 mediaSubtype = 0; // 1: photo, 2: video, 3: file, 4: voice, 5: music, 6: animation, 7: sticker.
		int64 mediaSize = 0;
		int64 duration = 0;
		int32 width = 0;
		int32 height = 0;
		QString fileName;
		QString originalFileName; // Version 4: source name when display title differs.
		QString mimeType;
		QString replyPreview;
		std::vector<Reaction> reactions;
		bool mediaPruned = false;
		TimeId date = 0;
		TimeId edited = 0; // Last locally observed content change.
		QString text;
		QString localMediaPath; // Version 2: validated Elitegram-owned file leaf.
		std::vector<Entity> entities;
		bool deleted = false;
	};

	explicit DeletedMessagesStore(not_null<Main::Session*> session);
	~DeletedMessagesStore();
	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);
	[[nodiscard]] int mediaLimitIndex() const;
	[[nodiscard]] rpl::producer<int> mediaLimitValue() const;
	void setMediaLimitIndex(int index);
	void clearArchive();

	void snapshot(
		not_null<HistoryItem*> item,
		bool allowBackgroundDownload = false);
	void promote(
		PeerId peer,
		MsgId message,
		HistoryItem *item,
		const char *source = "update");
	void flushDeletes();
	void clearForLogout();
	[[nodiscard]] std::vector<Record> deletedFor(
		PeerId peer,
		MsgId topic = MsgId()) const;
	[[nodiscard]] std::optional<Record> findDeleted(
		PeerId peer,
		MsgId topic,
		MsgId message) const;
	[[nodiscard]] rpl::producer<Key> deletedChanges() const;

private:
	[[nodiscard]] bool privatePeer(PeerId peer) const;
	[[nodiscard]] bool pruneNonPrivate();
	[[nodiscard]] bool eligible(not_null<const HistoryItem*> item) const;
	[[nodiscard]] const char *eligibilityReason(
		not_null<const HistoryItem*> item) const;
	void pruneActive();
	void refreshLocalMedia();
	void captureLocalMedia(
		not_null<HistoryItem*> item,
		const Key &key,
		bool allowBackgroundDownload = false);
	void queueMediaCopy(
		const Key &key,
		uint64 mediaId,
		uint8 mediaSubtype,
		QString source,
		QByteArray bytes,
		QString extension,
		QImage preview);
	void pruneMedia();
	[[nodiscard]] QString ownedMediaPath(const QString &leaf) const;
	void scheduleWrite();
	void write(bool sync);
	void load();

	const not_null<Main::Session*> _session;
	rpl::variable<bool> _enabled;
	rpl::variable<int> _mediaLimitIndex;
	std::map<Key, Record> _records;
	rpl::event_stream<Key> _deletedChanges;
	std::vector<Key> _pendingDeletedKeys;
	std::set<Key> _pendingMedia;
	std::set<Key> _photoNotAvailableLogged;
	std::map<Key, std::shared_ptr<Data::PhotoMedia>> _activePhotoViews;
	std::map<Key, bool> _photoEventFull;
	std::set<Key> _photoBackgroundLoadAttempted;
	std::map<Key, std::shared_ptr<Data::DocumentMedia>> _activeDocumentViews;
	std::set<Key> _documentBackgroundLoadAttempted;
	std::set<Key> _documentCompletionLogged;
	std::set<Key> _documentNotAvailableLogged;
	std::map<Key, int> _documentProgressBucket;
	std::set<Key> _documentFailureLogged;
	std::shared_ptr<std::atomic_bool> _mediaCancelled
		= std::make_shared<std::atomic_bool>(false);
	std::shared_ptr<std::atomic_uint> _mediaGeneration
		= std::make_shared<std::atomic_uint>(0);
	base::Timer _writeTimer;
	base::Timer _mediaRefreshTimer;
	rpl::lifetime _lifetime;
	bool _dirty = false;
	bool _pendingPromotion = false;
	int _snapshotDiagnosticCount = 0;
	int _promoteDiagnosticCount = 0;
	int _eventDiagnosticCount = 0;
	mutable int _queryDiagnosticCount = 0;
	int _persistDiagnosticCount = 0;
	mutable int _mediaDiagnosticCount = 0;
};

} // namespace Elitegram
