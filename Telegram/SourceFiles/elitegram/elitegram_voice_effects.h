/*
This file is part of Elitegram Desktop, based on Telegram Desktop.
The streaming DSP follows Elitegram Android's VoiceEffectsProcessor.
*/
#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Elitegram {

class VoiceEffectsProcessor final {
public:
	static constexpr int kSampleRate = 48000;
	enum Effect { Normal, Child, Adult, Robot, Echo, Female, Count };

	void process(int16_t *samples, int count, Effect effect) {
		if (effect < Normal || effect >= Count) effect = Normal;
		if (effect != _active) {
			_previous = _active;
			_previousPitchSlot = _activePitchSlot;
			_active = effect;
			_transitionRemaining = kTransitionSamples;
			_transitionDelay = 0;
			if (usesPitch(effect)) {
				_activePitchSlot = 1 - _activePitchSlot;
				_pitch[_activePitchSlot].reset();
				_tone[_activePitchSlot] = {};
				if (effect == Female) {
					_female[_activePitchSlot].reset();
				}
				_transitionDelay = PhaseVocoder::kWarmupSamples;
			} else if (effect == Echo) {
				_echo.fill(0);
				_echoIndex = 0;
			} else if (effect == Robot) {
				_robotDepth = 0.f;
				_robotComb.fill(0.f);
				_robotCombIndex = 0;
			}
		}
		if (effect == Normal && !_transitionRemaining) return;
		for (auto i = 0; i != count; ++i) {
			const auto input = float(samples[i]);
			const auto oldOutput = _transitionRemaining
				? ((_previous == Normal)
					? input
					: float(softLimit(render(input, _previous, _previousPitchSlot))))
				: input;
			const auto newOutput = render(input, _active, _activePitchSlot);
			auto output = newOutput;
			if (_transitionRemaining > 0) {
				const auto next = (_active == Normal)
					? input : float(softLimit(newOutput));
				if (_transitionDelay > 0) {
					--_transitionDelay;
					output = oldOutput;
				} else {
					const auto progress = 1.f - _transitionRemaining
						/ float(kTransitionSamples);
					const auto smooth = progress * progress * (3.f - 2.f * progress);
					output = oldOutput + (next - oldOutput) * smooth;
					--_transitionRemaining;
				}
				samples[i] = int16_t(std::lround(std::clamp(
					output, -32768.f, 32767.f)));
			} else {
				samples[i] = softLimit(output);
			}
		}
	}

	void reset() {
		if (_active != Normal || _transitionRemaining) {
			resetState();
		}
	}

private:
	static constexpr auto kPi = 3.14159265358979323846;
	static constexpr auto kTwoPi = 2.0 * kPi;
	static constexpr int kTransitionSamples = 720; // 15 ms at 48 kHz.
	static bool usesPitch(Effect effect) {
		return effect == Child || effect == Adult || effect == Female;
	}
	static int16_t softLimit(float sample) {
		const auto absolute = std::abs(sample);
		if (absolute <= 27000.f) return int16_t(std::lround(sample));
		const auto excess = absolute - 27000.f;
		const auto limited = 27000.f + 5767.f * excess / (excess + 5767.f);
		return int16_t(std::lround(std::copysign(limited, sample)));
	}

	class FemaleAnalysis final {
	public:
		float process(float input) {
			_dc += 0.005f * (input - _dc);
			const auto clean = input - _dc;
			_decimateSum += clean;
			if (++_decimateCount == 4) {
				_history[_write] = _decimateSum * 0.25f;
				_write = (_write + 1) & 511;
				_decimateSum = 0.f;
				_decimateCount = 0;
				if (_stored < 512) ++_stored;
				if (_stored == 512) {
					if (_untilAnalysis == 0) {
						analyze();
						_untilAnalysis = 120;
					}
					--_untilAnalysis;
				}
			}
			_pitchRatio += 0.0015f * (_targetPitch - _pitchRatio);
			_formantRatio += 0.001f * (_targetFormant - _formantRatio);
			_voicing += 0.0015f * (_targetVoicing - _voicing);
			return clean;
		}
		void reset() {
			_history.fill(0.f);
			_correlations.fill(0.f);
			_write = _stored = _decimateCount = _untilAnalysis = 0;
			_missedAnalyses = 0;
			_dc = _decimateSum = _voicing = _targetVoicing = 0.f;
			_pitchRatio = _targetPitch = 1.31f;
			_formantRatio = _targetFormant = 1.17f;
			_f0 = 0.f;
		}
		float pitchRatio() const { return _pitchRatio; }
		float formantRatio() const { return _formantRatio; }
		float voicing() const { return _voicing; }
	private:
		void analyze() {
			float mean = 0.f;
			for (const auto value : _history) mean += value;
			mean /= 512.f;
			float energy = 0.f;
			for (const auto value : _history) {
				const auto centered = value - mean;
				energy += centered * centered;
			}
			if (energy < 512.f * 180.f * 180.f) {
				reject();
				return;
			}
			float best = 0.f;
			int bestLag = 0;
			for (auto lag = 46; lag <= 171; ++lag) {
				double cross = 0., left = 0., right = 0.;
				for (auto i = lag; i != 512; ++i) {
					const auto a = _history[(_write + i) & 511] - mean;
					const auto b = _history[(_write + i - lag) & 511] - mean;
					cross += a * b;
					left += a * a;
					right += b * b;
				}
				const auto correlation = float(cross /
					std::sqrt(left * right + 1.));
				_correlations[lag] = correlation;
				if (correlation > best) {
					best = correlation;
					bestLag = lag;
				}
			}
			if (best < 0.68f) {
				reject();
				return;
			}
			if (_f0 != 0.f) {
				float bestScore = -1.f;
				for (auto lag = 47; lag < 171; ++lag) {
					if (_correlations[lag] < best - 0.06f
						|| _correlations[lag] < _correlations[lag - 1]
						|| _correlations[lag] < _correlations[lag + 1]) {
						continue;
					}
					const auto frequency = 12000.f / lag;
					const auto score = _correlations[lag]
						- 0.08f * std::abs(std::log2(frequency / _f0));
					if (score > bestScore) {
						bestScore = score;
						bestLag = lag;
					}
				}
			}
			const auto detected = 12000.f / bestLag;
			if (_f0 != 0.f
				&& (detected < _f0 * 0.75f || detected > _f0 * 1.33f)) {
				reject();
				return;
			}
			_f0 = (_f0 == 0.f) ? detected : 0.45f * _f0 + 0.55f * detected;
			_targetPitch = std::clamp(
				(195.f + 0.55f * (_f0 - 145.f)) / _f0,
				1.f, 1.65f);
			_targetFormant = std::clamp(
				1.19f - 0.00035f * (_f0 - 120.f), 1.12f, 1.20f);
			_targetVoicing = 1.f;
			_missedAnalyses = 0;
		}
		void reject() {
			_targetVoicing = 0.f;
			if (++_missedAnalyses > 20) _f0 = 0.f;
		}
		std::array<float, 512> _history{};
		std::array<float, 172> _correlations{};
		int _write = 0, _stored = 0, _decimateCount = 0;
		int _untilAnalysis = 0, _missedAnalyses = 0;
		float _dc = 0.f, _decimateSum = 0.f;
		float _f0 = 0.f, _voicing = 0.f, _targetVoicing = 0.f;
		float _pitchRatio = 1.31f, _targetPitch = 1.31f;
		float _formantRatio = 1.17f, _targetFormant = 1.17f;
	};

	class PhaseVocoder final {
	public:
		static constexpr int kWarmupSamples = 1152;
		PhaseVocoder() {
			for (auto i = 0; i != kFft; ++i) {
				_window[i] = std::sqrt(0.5 - 0.5 * std::cos(
					kTwoPi * i / (kFft - 1)));
				uint32_t bits = uint32_t(i), reverse = 0;
				for (auto n = 0; n != 10; ++n) {
					reverse = (reverse << 1) | (bits & 1);
					bits >>= 1;
				}
				_reverse[i] = int(reverse);
			}
			for (auto i = 0; i != kFft / 2; ++i) {
				_cosine[i] = std::cos(kTwoPi * i / kFft);
				_sine[i] = std::sin(kTwoPi * i / kFft);
			}
			for (auto i = 0; i != kFft; ++i) {
				const auto quefrency = std::min(i, kFft - i);
				_lifter[i] = (quefrency <= 48) ? 1.
					: (quefrency >= 72) ? 0.
					: 0.5 * (1. + std::cos(
						kPi * (quefrency - 48) / 24.));
			}
		}

		float process(float input, double pitch, double formant,
				double envelopeLimit = 0.45,
				bool cepstralEnvelope = false,
				double phaseLock = 0.) {
			const auto position = _sampleCount++;
			_input[position & kFftMask] = input;
			if (_sampleCount >= kFft
				&& ((_sampleCount - kFft) & (kHop - 1)) == 0) {
				synthesize(pitch, formant, envelopeLimit,
					cepstralEnvelope, phaseLock);
			}
			const auto index = position & kOutputMask;
			const auto weight = _outputWeight[index];
			const auto shifted = weight > 1.e-5f
				? _output[index] / weight : input;
			_output[index] = _outputWeight[index] = 0.f;
			return (position < kWarmupSamples) ? input : shifted;
		}

		void reset() {
			_input.fill(0.f);
			_output.fill(0.f);
			_outputWeight.fill(0.f);
			_previousPhase.fill(0.);
			_synthesisPhase.fill(0.);
			_smoothedEnvelope.fill(0.);
			_sampleCount = _frameCount = 0;
		}

	private:
		static constexpr int kFft = 1024, kFftMask = 1023;
		static constexpr int kHop = 256, kBins = 513;
		static constexpr int kOutputSize = 4096, kOutputMask = 4095;
		static constexpr int kEnvelopeRadius = 8;
		static double wrap(double phase) {
			return phase - kTwoPi * std::floor((phase + kPi) / kTwoPi);
		}
		void transform(bool inverse) {
			for (auto i = 0; i != kFft; ++i) {
				const auto reversed = _reverse[i];
				if (reversed > i) {
					std::swap(_real[i], _real[reversed]);
					std::swap(_imaginary[i], _imaginary[reversed]);
				}
			}
			for (auto length = 2; length <= kFft; length <<= 1) {
				const auto half = length >> 1;
				const auto step = kFft / length;
				for (auto start = 0; start < kFft; start += length) {
					for (auto offset = 0; offset < half; ++offset) {
						const auto table = offset * step;
						const auto wr = _cosine[table];
						const auto wi = inverse ? _sine[table] : -_sine[table];
						const auto even = start + offset, odd = even + half;
						const auto real = _real[odd] * wr - _imaginary[odd] * wi;
						const auto imag = _real[odd] * wi + _imaginary[odd] * wr;
						_real[odd] = _real[even] - real;
						_imaginary[odd] = _imaginary[even] - imag;
						_real[even] += real;
						_imaginary[even] += imag;
					}
				}
			}
			if (inverse) {
				for (auto i = 0; i != kFft; ++i) {
					_real[i] /= kFft;
					_imaginary[i] /= kFft;
				}
			}
		}
		void addDestination(int bin, int source, double magnitude,
				double frequency, double phase) {
			_destMagnitude[bin] += magnitude;
			_destFrequency[bin] += magnitude * frequency;
			_destSeedReal[bin] += magnitude * std::cos(phase);
			_destSeedImaginary[bin] += magnitude * std::sin(phase);
			if (magnitude > _destDominance[bin]) {
				_destDominance[bin] = magnitude;
				_destSource[bin] = source;
			}
		}
		double envelope(double index) const {
			if (index <= 0) return _smoothedEnvelope[0];
			if (index >= kBins - 1) return _smoothedEnvelope[kBins - 1];
			const auto lower = int(index);
			const auto fraction = index - lower;
			return _smoothedEnvelope[lower]
				+ (_smoothedEnvelope[lower + 1] - _smoothedEnvelope[lower]) * fraction;
		}
		void synthesize(double pitch, double formant,
				double envelopeLimit, bool cepstralEnvelope,
				double phaseLock) {
			const auto start = _sampleCount - kFft;
			for (auto i = 0; i != kFft; ++i) {
				_real[i] = _input[(start + i) & kFftMask] * _window[i];
				_imaginary[i] = 0.;
			}
			transform(false);
			for (auto bin = 0; bin != kBins; ++bin) {
				const auto magnitude = std::hypot(_real[bin], _imaginary[bin]);
				const auto phase = std::atan2(_imaginary[bin], _real[bin]);
				_sourceMagnitude[bin] = magnitude;
				_sourcePhase[bin] = phase;
				_logMagnitude[bin] = std::log1p(magnitude);
				const auto nominal = kTwoPi * kHop * bin / kFft;
				_sourceFrequency[bin] = (_frameCount == 0)
					? kTwoPi * bin / kFft
					: kTwoPi * bin / kFft
						+ wrap(phase - _previousPhase[bin] - nominal) / kHop;
				_previousPhase[bin] = phase;
			}
			if (cepstralEnvelope) {
				for (auto bin = 0; bin != kBins; ++bin) {
					_real[bin] = std::log(std::max(
						_sourceMagnitude[bin], 0.01));
					_imaginary[bin] = 0.;
				}
				for (auto bin = 1; bin < kFft / 2; ++bin) {
					_real[kFft - bin] = _real[bin];
					_imaginary[kFft - bin] = 0.;
				}
				transform(true);
				for (auto i = 0; i != kFft; ++i) {
					_real[i] *= _lifter[i];
					_imaginary[i] = 0.;
				}
				transform(false);
			} else {
				_envelopePrefix[0] = 0.;
				for (auto bin = 0; bin != kBins; ++bin) {
					_envelopePrefix[bin + 1] = _envelopePrefix[bin]
						+ _logMagnitude[bin];
				}
			}
			for (auto bin = 0; bin != kBins; ++bin) {
				if (cepstralEnvelope) {
					_spectralEnvelope[bin] = _real[bin];
				} else {
					const auto from = std::max(0, bin - kEnvelopeRadius);
					const auto to = std::min(kBins - 1, bin + kEnvelopeRadius);
					_spectralEnvelope[bin] = (_envelopePrefix[to + 1]
						- _envelopePrefix[from]) / (to - from + 1);
				}
				_smoothedEnvelope[bin] = (_frameCount == 0)
					? _spectralEnvelope[bin]
					: 0.65 * _smoothedEnvelope[bin] + 0.35 * _spectralEnvelope[bin];
			}
			_destMagnitude.fill(0.);
			_destFrequency.fill(0.);
			_destSeedReal.fill(0.);
			_destSeedImaginary.fill(0.);
			_destDominance.fill(0.);
			for (auto source = 0; source != kBins; ++source) {
				const auto target = source * pitch;
				const auto lower = int(target);
				if (lower >= kBins) break;
				const auto fraction = target - lower;
				const auto desired = envelope(target / formant);
				const auto correction = std::exp(std::clamp(
					desired - _smoothedEnvelope[source],
					-envelopeLimit, envelopeLimit));
				const auto magnitude = _sourceMagnitude[source] * correction;
				const auto frequency = _sourceFrequency[source] * pitch;
				addDestination(lower, source, magnitude * (1. - fraction),
					frequency, _sourcePhase[source]);
				if (fraction > 0. && lower + 1 < kBins) {
					addDestination(lower + 1, source, magnitude * fraction,
						frequency, _sourcePhase[source]);
				}
			}
			for (auto bin = 0; bin != kBins; ++bin) {
				const auto magnitude = _destMagnitude[bin];
				if (magnitude > 1.e-9) {
					_synthesisPhase[bin] = (_frameCount == 0)
						? std::atan2(_destSeedImaginary[bin], _destSeedReal[bin])
						: wrap(_synthesisPhase[bin]
							+ _destFrequency[bin] / magnitude * kHop);
				} else if (_frameCount != 0) {
					_synthesisPhase[bin] = wrap(_synthesisPhase[bin]
						+ kTwoPi * kHop * bin / kFft);
				}
				_independentPhase[bin] = _synthesisPhase[bin];
			}
			if (phaseLock > 0.001) {
				for (auto bin = 0; bin != kBins; ++bin) {
					const auto from = std::max(0, bin - 3);
					const auto to = std::min(kBins - 1, bin + 3);
					auto peak = bin;
					for (auto candidate = from; candidate <= to; ++candidate) {
						if (_sourceMagnitude[candidate] > _sourceMagnitude[peak]) {
							peak = candidate;
						}
					}
					_sourcePeak[bin] = peak;
				}
				for (auto bin = 1; bin < kFft / 2; ++bin) {
					if (_destMagnitude[bin] <= 1.e-9) continue;
					const auto source = _destSource[bin];
					const auto peak = _sourcePeak[source];
					const auto targetPeak = int(std::lround(peak * pitch));
					if (targetPeak <= 0 || targetPeak >= kBins
						|| targetPeak == bin
						|| _destMagnitude[targetPeak] <= 1.e-9) continue;
					const auto locked = wrap(_independentPhase[targetPeak]
						+ wrap(_sourcePhase[source] - _sourcePhase[peak]));
					_synthesisPhase[bin] = wrap(_independentPhase[bin]
						+ phaseLock * wrap(locked - _independentPhase[bin]));
				}
			}
			_real.fill(0.);
			_imaginary.fill(0.);
			for (auto bin = 0; bin != kBins; ++bin) {
				if (_destMagnitude[bin] > 1.e-9) {
					_real[bin] = _destMagnitude[bin]
						* std::cos(_synthesisPhase[bin]);
					_imaginary[bin] = _destMagnitude[bin]
						* std::sin(_synthesisPhase[bin]);
				}
			}
			_imaginary[0] = _imaginary[kFft / 2] = 0.;
			for (auto bin = 1; bin < kFft / 2; ++bin) {
				_real[kFft - bin] = _real[bin];
				_imaginary[kFft - bin] = -_imaginary[bin];
			}
			transform(true);
			const auto outputStart = _sampleCount & kOutputMask;
			for (auto i = 0; i != kFft; ++i) {
				const auto index = (outputStart + i) & kOutputMask;
				const auto window = float(_window[i]);
				_output[index] += float(_real[i]) * window;
				_outputWeight[index] += window * window;
			}
			++_frameCount;
		}
		std::array<float, kFft> _input{};
		std::array<float, kOutputSize> _output{}, _outputWeight{};
		std::array<double, kFft> _window{}, _real{}, _imaginary{}, _lifter{};
		std::array<double, kBins> _sourceMagnitude{}, _sourcePhase{},
			_sourceFrequency{}, _previousPhase{}, _synthesisPhase{},
			_logMagnitude{}, _spectralEnvelope{}, _smoothedEnvelope{},
			_destMagnitude{},
			_destFrequency{}, _destSeedReal{}, _destSeedImaginary{},
			_destDominance{}, _independentPhase{};
		std::array<int, kBins> _sourcePeak{}, _destSource{};
		std::array<double, kBins + 1> _envelopePrefix{};
		std::array<double, kFft / 2> _cosine{}, _sine{};
		std::array<int, kFft> _reverse{};
		int64_t _sampleCount = 0;
		int _frameCount = 0;
	};

	struct ToneState {
		float low = 0.f;
		float chest = 0.f;
		float body = 0.f;
		float presence = 0.f;
		float air = 0.f;
		float originalLow = 0.f;
		float transientEnvelope = 0.f;
		float transientGain = 0.f;
	};
	float shapeChild(float sample, ToneState &tone) {
		tone.body += 0.145f * (sample - tone.body);
		return sample * 0.92f + (sample - tone.body) * 0.13f;
	}
	float shapeAdult(float sample, ToneState &tone) {
		tone.low += 0.063f * (sample - tone.low);
		return sample * 0.76f + tone.low * 0.28f;
	}
	float shapeFemale(float sample, float original, float voicing,
			ToneState &tone) {
		tone.low += 0.019f * (sample - tone.low);
		tone.chest += 0.043f * (sample - tone.chest);
		tone.body += 0.22f * (sample - tone.body);
		tone.presence += 0.39f * (sample - tone.presence);
		tone.air += 0.63f * (sample - tone.air);
		tone.originalLow += 0.38f * (original - tone.originalLow);
		const auto highResidual = original - tone.originalLow;
		const auto highLevel = std::abs(highResidual);
		const auto transient = std::clamp(
			(highLevel - tone.transientEnvelope)
				/ (tone.transientEnvelope + 300.f), 0.f, 1.f);
		tone.transientEnvelope += 0.002f * (
			highLevel - tone.transientEnvelope);
		tone.transientGain += (transient > tone.transientGain
			? 0.09f : 0.006f) * (transient - tone.transientGain);
		const auto unvoiced = (1.f - voicing) * std::clamp(
			highLevel / (std::abs(original) + 300.f), 0.f, 1.f);
		return sample * 0.94f
			- (tone.chest - tone.low) * 0.16f
			+ (tone.presence - tone.body) * 0.12f
			- (tone.air - tone.presence) * 0.045f
			+ highResidual * (tone.transientGain * 0.06f
				+ unvoiced * 0.45f);
	}
	float shapeRobot(float sample) {
		const auto carrier = float(std::sin(_robotPhase));
		const auto harmonic = float(std::sin(_robotPhase * 2. + 0.35));
		_robotPhase += kTwoPi * 110. / kSampleRate;
		if (_robotPhase >= kTwoPi) _robotPhase -= kTwoPi;
		_robotDepth += (0.80f - _robotDepth) * 0.006f;
		const auto modulated = sample * (0.28f
			+ 0.80f * carrier + 0.12f * harmonic);
		const auto delayed = _robotComb[_robotCombIndex];
		_robotComb[_robotCombIndex] = modulated + delayed * 0.18f;
		if (++_robotCombIndex == int(_robotComb.size())) {
			_robotCombIndex = 0;
		}
		const auto wet = modulated + delayed * 0.12f;
		return sample * (1.f - _robotDepth) + wet * _robotDepth;
	}
	float applyEcho(float sample) {
		const auto delayed = float(_echo[(_echoIndex - 5280) & 8191]);
		const auto output = sample * 0.84f + delayed * 0.44f;
		_echo[_echoIndex] = softLimit(sample + delayed * 0.24f);
		_echoIndex = (_echoIndex + 1) & 8191;
		return output;
	}
	float render(float input, Effect effect, int slot) {
		switch (effect) {
		case Child:
			return shapeChild(_pitch[slot].process(input, 1.20, 1.08),
				_tone[slot]) * 1.52f;
		case Adult:
			return shapeAdult(_pitch[slot].process(input, 0.87, 0.95),
				_tone[slot]) * 1.42f;
		case Female: {
			const auto clean = _female[slot].process(input);
			const auto voicing = _female[slot].voicing();
			return shapeFemale(_pitch[slot].process(clean,
				_female[slot].pitchRatio(),
				_female[slot].formantRatio(), 0.65, true,
				0.60 * voicing), clean, voicing,
				_tone[slot]) * 1.26f;
		}
		case Robot: return shapeRobot(input) * 1.40f;
		case Echo: return applyEcho(input);
		default: return input;
		}
	}
	void resetState() {
		for (auto &pitch : _pitch) pitch.reset();
		for (auto &female : _female) female.reset();
		_echo.fill(0);
		_robotComb.fill(0.f);
		_tone = {};
		_echoIndex = _transitionRemaining = _transitionDelay = 0;
		_robotCombIndex = 0;
		_activePitchSlot = _previousPitchSlot = 0;
		_robotPhase = 0.;
		_robotDepth = 0.f;
		_active = _previous = Normal;
	}
	std::array<PhaseVocoder, 2> _pitch;
	std::array<FemaleAnalysis, 2> _female;
	std::array<ToneState, 2> _tone{};
	std::array<int16_t, 8192> _echo{};
	std::array<float, 72> _robotComb{};
	int _echoIndex = 0;
	int _robotCombIndex = 0;
	double _robotPhase = 0.;
	float _robotDepth = 0.f;
	int _transitionRemaining = 0, _transitionDelay = 0;
	int _activePitchSlot = 0, _previousPitchSlot = 0;
	Effect _active = Normal, _previous = Normal;
};

} // namespace Elitegram
