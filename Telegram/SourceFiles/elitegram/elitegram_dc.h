/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#pragma once

#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "ui/image/image_location.h"

#include <optional>
#include <variant>

namespace Elitegram {

[[nodiscard]] inline std::optional<MTP::DcId> ResolveSelfDc(
		not_null<Main::Session*> session) {
	if (session->account().mtp().isKeysDestroyer()) {
		return std::nullopt;
	}
	const auto dc = session->account().mtp().mainDcId();
	return dc > 0 ? std::optional<MTP::DcId>(dc) : std::nullopt;
}

[[nodiscard]] inline std::optional<MTP::DcId> ResolvePeerDc(
		not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return ResolveSelfDc(&peer->session());
	}
	const auto stats = peer->owner().statsDcId(peer);
	if (stats > 0) {
		return stats;
	}
	const auto userpic = peer->userpicLocation();
	const auto &location = userpic.file().data;
	if (const auto storage = std::get_if<StorageFileLocation>(&location)) {
		const auto dc = storage->dcId();
		if (storage->valid() && dc > 0) {
			return dc;
		}
	}
	return std::nullopt;
}

[[nodiscard]] inline QString FormatDc(MTP::DcId dc) {
	return u"DC %1"_q.arg(dc);
}

} // namespace Elitegram
