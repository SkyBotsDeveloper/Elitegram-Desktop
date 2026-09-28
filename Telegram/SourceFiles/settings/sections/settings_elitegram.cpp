/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
*/
#include "settings/sections/settings_elitegram.h"

#include "elitegram/elitegram_privacy.h"
#include "elitegram/elitegram_deleted_messages.h"
#include "elitegram/elitegram_outgoing_audio.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "styles/style_menu_icons.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <vector>

namespace Settings {
namespace {

using namespace Settings::Builder;

const std::vector<QString> &DeletedMediaLimits() {
	static const auto result = std::vector<QString>{
		u"512 MB"_q,
		u"1 GB"_q,
		u"2 GB"_q,
		u"5 GB"_q,
		u"Unlimited"_q,
	};
	return result;
}

const std::vector<QString> &VoiceEffects() {
	static const auto result = std::vector<QString>{
		u"Normal"_q, u"Child"_q, u"Adult"_q,
		u"Robot"_q, u"Echo"_q, u"Female"_q,
	};
	return result;
}

[[nodiscard]] rpl::producer<QString> ProtectionLabel(
		rpl::producer<bool> enabled) {
	return std::move(enabled) | rpl::map([](bool enabled) {
		return enabled ? u"Protected"_q : u"Off"_q;
	});
}

void BuildPrivacy(SectionBuilder &builder) {
	const auto privacy = &builder.session()->elitegramPrivacy();
	const auto deleted = &builder.session()->deletedMessages();
	const auto controller = builder.controller();
	builder.addSubsectionTitle(rpl::single(u"Privacy"_q));
	builder.addButton({
		.id = u"elitegram/ghost"_q,
		.title = rpl::single(u"Ghost Mode"_q),
		.icon = { &st::menuIconLock },
		.toggled = privacy->ghostEnabledValue(),
		.toggleIgnoreClick = true,
		.onClick = [=] {
			privacy->setGhostEnabled(!privacy->ghostEnabled());
		},
		.keywords = { u"ghost"_q, u"privacy"_q, u"offline"_q },
	});
	builder.addDividerText(rpl::single(
		u"Ghost Mode keeps local reading and composing usable while suppressing outgoing privacy signals."_q));

	const auto addProtection = [&](QString title, QStringList keywords) {
		builder.addButton({
			.title = rpl::single(std::move(title)),
			.label = ProtectionLabel(privacy->ghostEnabledValue()),
			.keywords = std::move(keywords),
		});
	};
	addProtection(u"Hide Online Status"_q, { u"online"_q, u"presence"_q });
	addProtection(u"Hide Typing & Recording"_q, { u"typing"_q, u"recording"_q });
	addProtection(u"Read Messages Without Seen"_q, { u"read"_q, u"seen"_q });
	addProtection(u"Hide Story Views"_q, { u"stories"_q, u"views"_q });

	builder.addButton({
		.id = u"elitegram/seen-on-reply"_q,
		.title = rpl::single(u"Seen on Reply"_q),
		.toggled = privacy->seenOnReplyValue(),
		.toggleIgnoreClick = true,
		.onClick = [=] {
			privacy->setSeenOnReply(!privacy->seenOnReply());
		},
		.keywords = { u"seen"_q, u"reply"_q, u"read"_q },
		.shown = privacy->ghostEnabledValue(),
	});
	builder.addDividerText(rpl::single(
		u"After a message is sent successfully, acknowledge pending reads in that chat only."_q));

	builder.addSubsectionTitle(rpl::single(u"Messages"_q));
	builder.addButton({
		.id = u"elitegram/keep-deleted-messages"_q,
		.title = rpl::single(u"Keep Deleted Messages"_q),
		.toggled = deleted->enabledValue(),
		.toggleIgnoreClick = true,
		.onClick = [=] { deleted->setEnabled(!deleted->enabled()); },
		.keywords = { u"deleted"_q, u"private chats"_q },
	});
	builder.addDividerText(rpl::single(
		u"Keep deleted messages in private chats"_q));
	builder.addButton({
		.id = u"elitegram/deleted-media-storage"_q,
		.title = rpl::single(u"Deleted Media Storage"_q),
		.label = deleted->mediaLimitValue() | rpl::map([](int index) {
			return DeletedMediaLimits()[index];
		}),
		.onClick = [=] {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				SingleChoiceBox(box, {
					.title = rpl::single(u"Deleted Media Storage"_q),
					.options = DeletedMediaLimits(),
					.initialSelection = deleted->mediaLimitIndex(),
					.callback = [=](int index) {
						deleted->setMediaLimitIndex(index);
					},
				});
			}));
		},
		.keywords = { u"deleted"_q, u"media"_q, u"storage"_q },
	});
	builder.addDividerText(rpl::single(
		u"Per-account limit for locally retained private-chat media. Unlimited still requires free disk space."_q));
	builder.addButton({
		.id = u"elitegram/clear-deleted-archive"_q,
		.title = rpl::single(u"Clear Deleted Messages Archive"_q),
		.onClick = [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = rpl::single(
					u"Remove this account's locally retained deleted messages and media? Normal chats and other accounts are unaffected."_q),
				.confirmed = [=] { deleted->clearArchive(); },
				.confirmText = rpl::single(u"Clear Archive"_q),
			}));
		},
		.keywords = { u"deleted"_q, u"clear"_q, u"archive"_q },
	});

	builder.addSubsectionTitle(rpl::single(u"Calls & Voice"_q));
	builder.addButton({
		.id = u"elitegram/selected-audio"_q,
		.title = rpl::single(u"Selected Audio"_q),
		.toggled = Elitegram::OutgoingAudio::selectedEnabledValue(),
		.toggleIgnoreClick = true,
		.onClick = [] {
			Elitegram::OutgoingAudio::setSelectedEnabled(
				!Elitegram::OutgoingAudio::selectedEnabled());
		},
		.keywords = { u"selected audio"_q, u"call file"_q },
	});
	builder.addDividerText(rpl::single(
		u"Play a chosen local file into an active call instead of the microphone."_q));
	builder.addButton({
		.id = u"elitegram/device-audio"_q,
		.title = rpl::single(u"Device Audio"_q),
		.toggled = Elitegram::OutgoingAudio::deviceEnabledValue(),
		.toggleIgnoreClick = true,
		.onClick = [] {
			Elitegram::OutgoingAudio::setDeviceEnabled(
				!Elitegram::OutgoingAudio::deviceEnabled());
		},
		.keywords = { u"device audio"_q, u"system playback"_q },
	});
	builder.addDividerText(rpl::single(
		u"Mix system playback quietly with the microphone. Pauses for screen sharing with audio."_q));
	builder.addButton({
		.id = u"elitegram/voice-effects"_q,
		.title = rpl::single(u"Voice Effects"_q),
		.label = Elitegram::OutgoingAudio::voiceEffectValue()
			| rpl::map([](int value) { return VoiceEffects()[value]; }),
		.onClick = [=] {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				SingleChoiceBox(box, {
					.title = rpl::single(u"Voice Effects"_q),
					.options = VoiceEffects(),
					.initialSelection = Elitegram::OutgoingAudio::voiceEffect(),
					.callback = [](int value) {
						Elitegram::OutgoingAudio::setVoiceEffect(value);
					},
				});
			}));
		},
		.keywords = { u"voice"_q, u"effect"_q },
	});
	builder.addButton({
		.id = u"elitegram/no-mic-blink"_q,
		.title = rpl::single(u"No Mic Blink"_q),
		.toggled = Elitegram::OutgoingAudio::noMicBlinkValue(),
		.toggleIgnoreClick = true,
		.onClick = [] {
			Elitegram::OutgoingAudio::setNoMicBlink(
				!Elitegram::OutgoingAudio::noMicBlink());
		},
		.keywords = { u"mic"_q, u"group call"_q },
	});
	builder.addDividerText(rpl::single(
		u"In group calls, keep local mute working without sending ordinary later self mute changes."_q));

	builder.addSubsectionTitle(rpl::single(u"Appearance"_q));
	const auto session = builder.session();
	builder.addButton({
		.id = u"elitegram/show-dc"_q,
		.title = rpl::single(u"Show Data Center in Profiles"_q),
		.toggled = session->settings().elitegramShowDcValue(),
		.toggleIgnoreClick = true,
		.onClick = [=] {
			auto &settings = session->settings();
			settings.setElitegramShowDc(!settings.elitegramShowDc());
			session->saveSettingsDelayed();
		},
		.keywords = { u"data center"_q, u"dc"_q, u"profiles"_q },
	});
}

class ElitegramFeatures final : public Section<ElitegramFeatures> {
public:
	ElitegramFeatures(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
	: Section(parent, controller) {
		const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
		const SectionBuildMethod method = [](
				not_null<Ui::VerticalLayout*> container,
				not_null<Window::SessionController*> controller,
				Fn<void(Type)> showOther,
				rpl::producer<> showFinished) {
			const auto isPaused = Window::PausedIn(
				controller,
				Window::GifPauseReason::Layer);
			auto builder = SectionBuilder(WidgetContext{
				.container = container,
				.controller = controller,
				.showOther = std::move(showOther),
				.isPaused = isPaused,
			});
			BuildPrivacy(builder);
		};
		build(content, method);
		Ui::ResizeFitChild(this, content);
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return rpl::single(u"Elitegram Features"_q);
	}
};

} // namespace

Type ElitegramFeaturesId() {
	return ElitegramFeatures::Id();
}

} // namespace Settings
