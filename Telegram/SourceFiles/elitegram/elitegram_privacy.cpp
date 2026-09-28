/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#include "elitegram/elitegram_privacy.h"

#include "apiwrap.h"
#include "api/api_updates.h"
#include "data/data_channel.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"

namespace Elitegram {

PrivacyController::PrivacyController(not_null<Main::Session*> session)
: _session(session)
, _ghostEnabled(session->settings().elitegramGhostEnabled())
, _seenOnReply(session->settings().elitegramSeenOnReply()) {
}

bool PrivacyController::ghostEnabled() const {
	return _ghostEnabled.current();
}

rpl::producer<bool> PrivacyController::ghostEnabledValue() const {
	return _ghostEnabled.value();
}

void PrivacyController::setGhostEnabled(bool enabled) {
	if (ghostEnabled() == enabled) {
		return;
	}
	if (!enabled) {
		releaseAllPendingReads();
	}
	_ghostEnabled = enabled;
	_session->settings().setElitegramGhostEnabled(enabled);
	_session->saveSettingsDelayed();
	if (enabled) {
		// Correct an already advertised online state immediately. Normal
		// connectivity remains active; only presence is changed.
		_session->api().request(MTPaccount_UpdateStatus(MTP_bool(true))).send();
	} else {
		_session->updates().updateOnline();
	}
}

bool PrivacyController::seenOnReply() const {
	return _seenOnReply.current();
}

rpl::producer<bool> PrivacyController::seenOnReplyValue() const {
	return _seenOnReply.value();
}

void PrivacyController::setSeenOnReply(bool enabled) {
	if (seenOnReply() == enabled) {
		return;
	}
	_seenOnReply = enabled;
	_session->settings().setElitegramSeenOnReply(enabled);
	_session->saveSettingsDelayed();
}

bool PrivacyController::suppressOnlinePresence() const {
	return ghostEnabled();
}

bool PrivacyController::suppressSendActions() const {
	return ghostEnabled();
}

bool PrivacyController::suppressReadAcknowledgements() const {
	return ghostEnabled();
}

bool PrivacyController::suppressStoryViews() const {
	return ghostEnabled();
}

void PrivacyController::deferRead(
		not_null<History*> history,
		MsgId tillId) {
	if (!IsServerMsgId(tillId)) {
		return;
	}
	auto &pending = _pendingReads[history->peer->id];
	pending = std::max(pending, tillId);
}

void PrivacyController::releasePendingRead(not_null<History*> history) {
	if (!seenOnReply() || !ghostEnabled()) {
		return;
	}
	sendPendingRead(history);
}

void PrivacyController::sendPendingRead(not_null<History*> history) {
	const auto peerId = history->peer->id;
	const auto i = _pendingReads.find(peerId);
	if (i == end(_pendingReads) || _readRequests.contains(peerId)) {
		return;
	}
	const auto tillId = i->second;
	_readRequests.emplace(peerId);
	const auto weak = base::make_weak(this);
	const auto done = [=] {
		if (!weak) {
			return;
		}
		weak->_readRequests.remove(peerId);
		const auto pending = weak->_pendingReads.find(peerId);
		if (pending != end(weak->_pendingReads) && pending->second <= tillId) {
			weak->_pendingReads.erase(pending);
		}
	};
	const auto fail = [=] {
		if (weak) {
			weak->_readRequests.remove(peerId);
		}
	};
	if (const auto channel = history->peer->asChannel()) {
		_session->api().request(MTPchannels_ReadHistory(
			channel->inputChannel(),
			MTP_int(tillId)
		)).done(done).fail(fail).send();
	} else {
		_session->api().request(MTPmessages_ReadHistory(
			history->peer->input(),
			MTP_int(tillId)
		)).done([=](const MTPmessages_AffectedMessages &result) {
			if (weak) {
				weak->_session->api().applyAffectedMessages(
					history->peer,
					result);
			}
			done();
		}).fail(fail).send();
	}
}

void PrivacyController::releaseAllPendingReads() {
	const auto pending = _pendingReads;
	for (const auto &[peerId, tillId] : pending) {
		sendPendingRead(_session->data().history(peerId));
	}
}

} // namespace Elitegram
