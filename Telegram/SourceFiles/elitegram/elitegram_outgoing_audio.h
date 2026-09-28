/* Elitegram call-scoped outgoing PCM processing. */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>

#include <QString>
#include "rpl/variable.h"

namespace Elitegram {

enum class SelectedState : uint8_t {
	Idle,
	Selecting,
	Preparing,
	Playing,
	Finished,
};

class OutgoingAudio final : public std::enable_shared_from_this<OutgoingAudio> {
public:
	static std::shared_ptr<OutgoingAudio> Create(bool group);
	~OutgoingAudio();

	// Main thread, except process() (the ADM capture thread).
	void start();
	void stop();
	void setMuted(bool muted);
	void pauseDeviceAudio(bool pause);
	void chooseSelectedAudio();
	void stopSelectedAudio();
	[[nodiscard]] SelectedState selectedState() const;
	void process(int16_t *pcm, int frames, int rate, int channels);
	void refreshPreferences();
	void setNoMicBlinkReconcile(std::function<void()> callback);
	void reconcileNoMicBlink();

	[[nodiscard]] static bool selectedEnabled();
	[[nodiscard]] static bool deviceEnabled();
	[[nodiscard]] static bool noMicBlink();
	[[nodiscard]] static int voiceEffect();
	[[nodiscard]] static rpl::producer<bool> selectedEnabledValue();
	[[nodiscard]] static rpl::producer<bool> deviceEnabledValue();
	[[nodiscard]] static rpl::producer<bool> noMicBlinkValue();
	[[nodiscard]] static rpl::producer<int> voiceEffectValue();
	static void setSelectedEnabled(bool enabled);
	static void setDeviceEnabled(bool enabled);
	static void setNoMicBlink(bool enabled);
	static void setVoiceEffect(int effect);

private:
	explicit OutgoingAudio(bool group);
	void beginSelectedAudio(const QString &path);
	void updateDeviceCapture();
	struct Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Elitegram
