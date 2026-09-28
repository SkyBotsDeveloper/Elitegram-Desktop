/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "rpl/variable.h"

class History;

namespace Main {
class Session;
} // namespace Main

namespace Elitegram {

class PrivacyController final : public base::has_weak_ptr {
public:
	explicit PrivacyController(not_null<Main::Session*> session);

	[[nodiscard]] bool ghostEnabled() const;
	[[nodiscard]] rpl::producer<bool> ghostEnabledValue() const;
	void setGhostEnabled(bool enabled);

	[[nodiscard]] bool seenOnReply() const;
	[[nodiscard]] rpl::producer<bool> seenOnReplyValue() const;
	void setSeenOnReply(bool enabled);

	[[nodiscard]] bool suppressOnlinePresence() const;
	[[nodiscard]] bool suppressSendActions() const;
	[[nodiscard]] bool suppressReadAcknowledgements() const;
	[[nodiscard]] bool suppressStoryViews() const;

	void deferRead(not_null<History*> history, MsgId tillId);
	void releasePendingRead(not_null<History*> history);

private:
	void sendPendingRead(not_null<History*> history);
	void releaseAllPendingReads();

	const not_null<Main::Session*> _session;
	rpl::variable<bool> _ghostEnabled;
	rpl::variable<bool> _seenOnReply;
	base::flat_map<PeerId, MsgId> _pendingReads;
	base::flat_set<PeerId> _readRequests;
};

} // namespace Elitegram
