/* Elitegram call-scoped outgoing PCM processing. */
#include "elitegram/elitegram_outgoing_audio.h"

#include "elitegram/elitegram_voice_effects.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "logs.h"
#include "ui/toast/toast.h"
#include "webrtc/webrtc_create_adm.h"
#include "crl/crl_async.h"

#include <QFile>
#include <QByteArray>

#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <thread>
#include <vector>

namespace Elitegram {
namespace {

constexpr auto kRate = 48000;
constexpr auto kFrame = 480;
constexpr auto kDeviceGain = 0.10f;

template <size_t Capacity>
class PcmRing final {
public:
	[[nodiscard]] size_t available() const {
		return _write.load(std::memory_order_acquire)
			- _read.load(std::memory_order_acquire);
	}
	[[nodiscard]] size_t free() const { return Capacity - available(); }
	size_t push(const int16_t *data, size_t count) {
		const auto write = _write.load(std::memory_order_relaxed);
		const auto read = _read.load(std::memory_order_acquire);
		const auto take = std::min(count, Capacity - (write - read));
		for (auto i = size_t(); i != take; ++i) {
			_data[(write + i) % Capacity] = data[i];
		}
		_write.store(write + take, std::memory_order_release);
		return take;
	}
	size_t pop(int16_t *data, size_t count) {
		const auto read = _read.load(std::memory_order_relaxed);
		const auto write = _write.load(std::memory_order_acquire);
		const auto take = std::min(count, write - read);
		for (auto i = size_t(); i != take; ++i) {
			data[i] = _data[(read + i) % Capacity];
		}
		_read.store(read + take, std::memory_order_release);
		return take;
	}
	void skip(size_t count) {
		const auto read = _read.load(std::memory_order_relaxed);
		const auto write = _write.load(std::memory_order_acquire);
		_read.store(read + std::min(count, write - read),
			std::memory_order_release);
	}
private:
	std::array<int16_t, Capacity> _data = {};
	std::atomic<size_t> _read = 0;
	std::atomic<size_t> _write = 0;
};

using Ring = PcmRing<kRate>; // Selected file: at most one second.
using DeviceRing = PcmRing<kRate / 5>; // Live system audio: at most 200 ms.

std::vector<std::weak_ptr<OutgoingAudio>> &Engines() {
	static auto result = std::vector<std::weak_ptr<OutgoingAudio>>();
	return result; // Main-thread registry; audio callbacks never touch it.
}

void RefreshEngines() {
	auto &engines = Engines();
	for (auto i = engines.begin(); i != engines.end();) {
		if (const auto engine = i->lock()) {
			engine->refreshPreferences();
			++i;
		} else {
			i = engines.erase(i);
		}
	}
}

rpl::variable<bool> &SelectedSetting() {
	static auto value = rpl::variable<bool>(Core::App().settings().readPref<bool>(
		"elitegram/selected-audio", false));
	return value;
}
rpl::variable<bool> &DeviceSetting() {
	static auto value = rpl::variable<bool>(Core::App().settings().readPref<bool>(
		"elitegram/device-audio", false));
	return value;
}
rpl::variable<bool> &NoBlinkSetting() {
	static auto value = rpl::variable<bool>(Core::App().settings().readPref<bool>(
		"elitegram/no-mic-blink", false));
	return value;
}
rpl::variable<int> &EffectSetting() {
	static auto value = rpl::variable<int>(std::clamp(
		Core::App().settings().readPref<QByteArray>(
			"elitegram/voice-effect", QByteArray("0")).toInt(),
		0, 5));
	return value;
}

struct FileIo final {
	QFile file;
	std::atomic<bool> *cancel = nullptr;
	AVIOContext *io = nullptr;
	uint8_t *buffer = nullptr;
	explicit FileIo(const QString &path, std::atomic<bool> *cancel)
	: file(path), cancel(cancel) {
		if (!file.open(QIODevice::ReadOnly)) return;
		buffer = static_cast<uint8_t*>(av_malloc(4096));
		if (buffer) {
			io = avio_alloc_context(buffer, 4096, 0, this, &Read, nullptr, &Seek);
			if (io) buffer = nullptr; // AVIOContext owns the buffer.
		}
	}
	~FileIo() {
		if (io) {
			av_freep(&io->buffer);
			avio_context_free(&io);
		} else if (buffer) {
			av_free(buffer);
		}
	}
	static int Read(void *opaque, uint8_t *dst, int count) {
		const auto self = static_cast<FileIo*>(opaque);
		if (self->cancel->load()) return AVERROR_EXIT;
		const auto got = self->file.read(
			reinterpret_cast<char*>(dst), count);
		return got > 0 ? int(got) : AVERROR_EOF;
	}
	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		const auto self = static_cast<FileIo*>(opaque);
		if (self->cancel->load()) return AVERROR_EXIT;
		auto &file = self->file;
		if (whence == AVSEEK_SIZE) return file.size();
		const auto base = ((whence & ~AVSEEK_FORCE) == SEEK_CUR)
			? file.pos() : ((whence & ~AVSEEK_FORCE) == SEEK_END)
			? file.size() : qint64(0);
		const auto target = base + offset;
		return (target >= 0 && file.seek(target)) ? target : -1;
	}
};

} // namespace

struct OutgoingAudio::Impl final {
	explicit Impl(bool group) : group(group) {}
	~Impl() { stop(); }

	void stopSelected(const char *reason) {
		generation.fetch_add(1, std::memory_order_acq_rel);
		decoderStop.store(true, std::memory_order_release);
		selected = SelectedState::Idle;
		if (decoder.joinable()) decoder.join();
		decoderDone = false;
		selectedRing.store(std::make_shared<Ring>());
		LOG(("ElitegramAudio selected_stop reason=%1"
			).arg(QString::fromLatin1(reason)));
	}
	void stopDevice() {
		deviceReady = false;
		if (device) {
			device->stop();
			device.reset();
			deviceRing.store(std::make_shared<DeviceRing>());
			LOG(("ElitegramAudio device_audio_stop"));
		}
	}
	void stop() {
		active = false;
		stopSelected("call_end");
		stopDevice();
	}
	void startDevice() {
		if (device) return;
		LOG(("ElitegramAudio device_audio_start"));
		device = std::make_unique<Webrtc::LoopbackAudioTap>(
			[this](const int16_t *samples, size_t frames,
					size_t bytesPerSample, size_t channels, uint32_t rate) {
				onDeviceAudio(samples, frames, bytesPerSample, channels, rate);
			});
		if (!device->start()) {
			LOG(("ElitegramAudio device_audio_failed reason=adm_start"));
			stopDevice();
			return;
		}
		deviceReady = true;
		LOG(("ElitegramAudio device_audio_ready"));
	}

	void onDeviceAudio(
			const int16_t *samples, size_t frames, size_t bytesPerSample,
			size_t channels, uint32_t rate) {
		if (!deviceReady || !active || muted || paused
			|| rate != kRate || channels != 2
			|| bytesPerSample != 2 * sizeof(int16_t)) return;
		std::array<int16_t, kFrame> mono;
		for (auto offset = size_t(); offset < frames; offset += kFrame) {
			const auto count = std::min(size_t(kFrame), frames - offset);
			for (auto i = size_t(); i != count; ++i) {
				const auto at = 2 * (offset + i);
				mono[i] = int16_t((int(samples[at]) + samples[at + 1]) / 2);
			}
			const auto ring = deviceRing.load();
			if (ring->free() < count) {
				// Do not block the WASAPI callback or grow the queue.
				break;
			}
			ring->push(mono.data(), count);
		}
	}

	void decode(const QString &path, uint64_t token) {
		FileIo input(path, &decoderStop);
		if (!input.io) return decodeFailed(token);
		auto format = avformat_alloc_context();
		if (!format) return decodeFailed(token);
		format->pb = input.io;
		format->flags |= AVFMT_FLAG_CUSTOM_IO;
		format->interrupt_callback.callback = [](void *opaque) {
			return static_cast<Impl*>(opaque)->decoderStop.load() ? 1 : 0;
		};
		format->interrupt_callback.opaque = this;
		FFmpeg::RestrictToCustomIO(format);
		if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
			avformat_free_context(format);
			return decodeFailed(token);
		}
		const auto close = [&] { avformat_close_input(&format); };
		if (avformat_find_stream_info(format, nullptr) < 0) {
			close(); return decodeFailed(token);
		}
		const auto stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
			-1, -1, nullptr, 0);
		if (stream < 0) { close(); return decodeFailed(token); }
		const auto params = format->streams[stream]->codecpar;
		const auto codec = avcodec_find_decoder(params->codec_id);
		if (!codec) { close(); return decodeFailed(token); }
		auto context = avcodec_alloc_context3(codec);
		if (!context || avcodec_parameters_to_context(context, params) < 0
			|| avcodec_open2(context, codec, nullptr) < 0) {
			avcodec_free_context(&context); close(); return decodeFailed(token);
		}
		auto swr = static_cast<SwrContext*>(nullptr);
		const auto mono = AVChannelLayout(AV_CHANNEL_LAYOUT_MONO);
		if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, kRate,
				&context->ch_layout, context->sample_fmt,
				context->sample_rate, 0, nullptr) < 0
			|| !swr || swr_init(swr) < 0) {
			swr_free(&swr); avcodec_free_context(&context); close();
			return decodeFailed(token);
		}
		auto packet = av_packet_alloc();
		auto frame = av_frame_alloc();
		if (!packet || !frame) {
			av_packet_free(&packet); av_frame_free(&frame); swr_free(&swr);
			avcodec_free_context(&context); close(); return decodeFailed(token);
		}
		const auto pushSamples = [&](const int16_t *pcm, size_t count) {
			for (auto offset = size_t(); offset < count
					&& !decoderStop && generation == token;) {
				const auto ring = selectedRing.load();
				const auto pushed = ring->push(pcm + offset, count - offset);
				offset += pushed;
				if (!pushed) std::this_thread::sleep_for(
					std::chrono::milliseconds(5));
			}
			if (selected == SelectedState::Preparing
				&& selectedRing.load()->available() >= kFrame) {
				selected = SelectedState::Playing;
				LOG(("ElitegramAudio selected_ready"));
			}
		};
		const auto appendFrame = [&] {
			const auto maximum = int(av_rescale_rnd(
				swr_get_delay(swr, context->sample_rate) + frame->nb_samples,
				kRate, context->sample_rate, AV_ROUND_UP));
			if (maximum <= 0 || maximum > 10 * kRate) return;
			auto pcm = std::vector<int16_t>(maximum);
			auto output = reinterpret_cast<uint8_t*>(pcm.data());
			const auto count = swr_convert(swr, &output, maximum,
				(const uint8_t**)frame->extended_data,
				frame->nb_samples);
			if (count > 0) pushSamples(pcm.data(), count);
		};
		const auto receive = [&] {
			while (avcodec_receive_frame(context, frame) == 0) {
				appendFrame();
				av_frame_unref(frame);
				if (decoderStop || generation != token) break;
			}
		};
		while (!decoderStop && generation == token
				&& av_read_frame(format, packet) >= 0) {
			if (packet->stream_index == stream) {
				auto sent = avcodec_send_packet(context, packet);
				if (sent == AVERROR(EAGAIN)) {
					receive();
					sent = avcodec_send_packet(context, packet);
				}
				if (sent == 0) receive();
			}
			av_packet_unref(packet);
		}
		if (!decoderStop && generation == token) {
			avcodec_send_packet(context, nullptr);
			receive();
			for (auto attempt = 0; attempt != 8; ++attempt) {
				const auto delayed = swr_get_delay(swr, context->sample_rate);
				if (delayed <= 0) break;
				const auto capacity = int(av_rescale_rnd(delayed, kRate,
					context->sample_rate, AV_ROUND_UP));
				if (capacity <= 0 || capacity > kRate) break;
				auto pcm = std::vector<int16_t>(capacity);
				auto output = reinterpret_cast<uint8_t*>(pcm.data());
				const auto produced = swr_convert(
					swr, &output, capacity, nullptr, 0);
				if (produced <= 0) break;
				pushSamples(pcm.data(), produced);
			}
			if (selected == SelectedState::Preparing) {
				const auto count = selectedRing.load()->available();
				if (count > 0 && count < kFrame) {
					std::array<int16_t, kFrame> silence = {};
					selectedRing.load()->push(silence.data(), kFrame - count);
					selected = SelectedState::Playing;
				}
			}
			decoderDone = true;
			if (selectedRing.load()->available() == 0) {
				selected = SelectedState::Finished;
			}
		}
		av_packet_free(&packet);
		av_frame_free(&frame);
		swr_free(&swr);
		avcodec_free_context(&context);
		close();
	}
	void decodeFailed(uint64_t token) {
		if (generation != token || decoderStop) return;
		selected = SelectedState::Finished;
		decoderDone = true;
		LOG(("ElitegramAudio selected_stop reason=error"));
		crl::on_main([] {
			Ui::Toast::Show(u"Could not play the selected audio file."_q);
		});
	}

	const bool group;
	std::atomic<bool> active = false;
	std::atomic<bool> muted = false;
	std::atomic<bool> paused = false;
	std::atomic<bool> deviceEnabled = false;
	std::atomic<bool> deviceReady = false;
	std::atomic<int> effect = 0;
	std::atomic<bool> formatLogged = false;
	std::atomic<bool> resetEffectPending = false;
	VoiceEffectsProcessor effectProcessor;
	std::atomic<std::shared_ptr<Ring>> selectedRing{ std::make_shared<Ring>() };
	std::atomic<std::shared_ptr<DeviceRing>> deviceRing
		{ std::make_shared<DeviceRing>() };
	std::atomic<SelectedState> selected = SelectedState::Idle;
	std::atomic<bool> decoderStop = false;
	std::atomic<bool> decoderDone = false;
	std::atomic<uint64_t> generation = 0;
	std::thread decoder;
	std::unique_ptr<Webrtc::LoopbackAudioTap> device;
	std::function<void()> reconcile;
};

OutgoingAudio::OutgoingAudio(bool group) : _impl(std::make_unique<Impl>(group)) {}
OutgoingAudio::~OutgoingAudio() = default;

std::shared_ptr<OutgoingAudio> OutgoingAudio::Create(bool group) {
	auto result = std::shared_ptr<OutgoingAudio>(new OutgoingAudio(group));
	Engines().emplace_back(result);
	result->refreshPreferences();
	LOG(("ElitegramAudio engine_create call=%1"
		).arg(group ? u"group"_q : u"private"_q));
	return result;
}

void OutgoingAudio::start() {
	_impl->active = true;
	updateDeviceCapture();
}
void OutgoingAudio::stop() { _impl->stop(); }
void OutgoingAudio::setMuted(bool muted) {
	_impl->muted = muted;
	if (muted) {
		_impl->stopSelected("mute");
		_impl->resetEffectPending = true;
	}
	updateDeviceCapture();
}
void OutgoingAudio::pauseDeviceAudio(bool pause) {
	_impl->paused = pause;
	updateDeviceCapture();
}
void OutgoingAudio::updateDeviceCapture() {
	if (_impl->active && !_impl->muted && !_impl->paused
		&& _impl->deviceEnabled && Webrtc::LoopbackAudioCaptureSupported()) {
		_impl->startDevice();
	} else {
		_impl->stopDevice();
	}
}
void OutgoingAudio::chooseSelectedAudio() {
	if (!selectedEnabled() || !_impl->active || _impl->muted) return;
	const auto token = ++_impl->generation;
	_impl->selected = SelectedState::Selecting;
	const auto weak = weak_from_this();
	FileDialog::GetOpenPath(Core::App().getFileDialogParent(),
		u"Play Selected Audio"_q, FileDialog::AllFilesFilter(), [weak, token](
			const FileDialog::OpenResult &result) {
		if (const auto strong = weak.lock()) {
			if (strong->_impl->generation != token) return;
			if (result.paths.isEmpty()) {
				strong->_impl->selected = SelectedState::Idle;
			} else {
				strong->beginSelectedAudio(result.paths.front());
			}
		}
	}, [weak, token] {
		if (const auto strong = weak.lock(); strong
			&& strong->_impl->generation == token) {
			strong->_impl->selected = SelectedState::Idle;
			Ui::Toast::Show(u"Could not open the audio file picker."_q);
		}
	});
}
void OutgoingAudio::beginSelectedAudio(const QString &path) {
	_impl->stopSelected("replaced");
	if (!_impl->active || _impl->muted || !selectedEnabled()) return;
	_impl->decoderStop = false;
	_impl->selected = SelectedState::Preparing;
	_impl->resetEffectPending = true;
	const auto token = ++_impl->generation;
	LOG(("ElitegramAudio selected_prepare"));
	_impl->decoder = std::thread([impl = _impl.get(), path, token] {
		impl->decode(path, token);
	});
}
void OutgoingAudio::stopSelectedAudio() { _impl->stopSelected("user"); }
SelectedState OutgoingAudio::selectedState() const { return _impl->selected; }

void OutgoingAudio::process(int16_t *pcm, int frames, int rate, int channels) {
	if (_impl->resetEffectPending.exchange(false)) {
		_impl->effectProcessor.reset();
	}
	if (!_impl->formatLogged.exchange(true)) {
		LOG(("ElitegramAudio format rate=%1 channels=%2 frame_ms=%3")
			.arg(rate).arg(channels).arg(frames * 1000 / std::max(rate, 1)));
	}
	if (!_impl->active) return;
	if (_impl->muted) {
		std::fill(pcm, pcm + frames * channels, int16_t(0));
		return;
	}
	if (rate != kRate || channels != 1 || frames != kFrame) return;
	const auto selected = _impl->selected.load();
	if (selected == SelectedState::Playing) {
		const auto ring = _impl->selectedRing.load();
		const auto read = ring->pop(pcm, frames);
		std::fill(pcm + read, pcm + frames, int16_t(0));
		if (_impl->decoderDone && !ring->available()) {
			_impl->selected = SelectedState::Finished;
		}
		return; // Selected file replaces mic and bypasses effect/system mix.
	}
	if (selected == SelectedState::Preparing) return;
	const auto effect = _impl->effect.load();
	_impl->effectProcessor.process(pcm, frames,
		VoiceEffectsProcessor::Effect(effect));
	if (_impl->deviceReady && !_impl->paused && _impl->deviceEnabled) {
		std::array<int16_t, kFrame> system = {};
		const auto ring = _impl->deviceRing.load();
		const auto available = ring->available();
		if (available > size_t(3 * frames)) {
			ring->skip(((available - 2 * frames) / frames) * frames);
		}
		if (ring->available() >= size_t(frames)
			&& ring->pop(system.data(), frames) == size_t(frames)) {
			for (auto i = 0; i != frames; ++i) {
				pcm[i] = int16_t(std::clamp(
					int(pcm[i]) + int(system[i] * kDeviceGain),
					-32768, 32767));
			}
		}
	}
}

void OutgoingAudio::refreshPreferences() {
	_impl->effect = EffectSetting().current();
	_impl->deviceEnabled = deviceEnabled();
	if (!selectedEnabled() && _impl->selected != SelectedState::Idle) {
		_impl->stopSelected("disabled");
	}
	updateDeviceCapture();
}
void OutgoingAudio::setNoMicBlinkReconcile(std::function<void()> callback) {
	_impl->reconcile = std::move(callback);
}
void OutgoingAudio::reconcileNoMicBlink() {
	if (_impl->reconcile) _impl->reconcile();
}

bool OutgoingAudio::selectedEnabled() {
	return SelectedSetting().current();
}
bool OutgoingAudio::deviceEnabled() {
	return DeviceSetting().current();
}
bool OutgoingAudio::noMicBlink() {
	return NoBlinkSetting().current();
}
int OutgoingAudio::voiceEffect() { return EffectSetting().current(); }
rpl::producer<bool> OutgoingAudio::selectedEnabledValue() {
	return SelectedSetting().value();
}
rpl::producer<bool> OutgoingAudio::deviceEnabledValue() {
	return DeviceSetting().value();
}
rpl::producer<bool> OutgoingAudio::noMicBlinkValue() {
	return NoBlinkSetting().value();
}
rpl::producer<int> OutgoingAudio::voiceEffectValue() {
	return EffectSetting().value();
}
void OutgoingAudio::setSelectedEnabled(bool value) {
	Core::App().settings().writePref<bool>("elitegram/selected-audio", value);
	SelectedSetting() = value;
	RefreshEngines();
}
void OutgoingAudio::setDeviceEnabled(bool value) {
	Core::App().settings().writePref<bool>("elitegram/device-audio", value);
	DeviceSetting() = value;
	RefreshEngines();
}
void OutgoingAudio::setNoMicBlink(bool value) {
	const auto was = noMicBlink();
	Core::App().settings().writePref<bool>("elitegram/no-mic-blink", value);
	NoBlinkSetting() = value;
	RefreshEngines();
	if (was && !value) {
		for (const auto &weak : Engines()) {
			if (const auto engine = weak.lock()) engine->reconcileNoMicBlink();
		}
	}
}
void OutgoingAudio::setVoiceEffect(int value) {
	value = std::clamp(value, 0, 5);
	Core::App().settings().writePref<QByteArray>(
		"elitegram/voice-effect", QByteArray::number(value));
	EffectSetting() = value;
	RefreshEngines();
	LOG(("ElitegramAudio effect_change effect=%1").arg(value));
}

} // namespace Elitegram
