#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace bbb::webrtc {

// Streaming mono polyphase FIR. The history and fractional input position
// persist across Max vector boundaries. Only setup builds the filter table.
class audio_rate_converter {
public:
	void setup(double input_rate, double output_rate) {
		input_rate_ = input_rate;
		output_rate_ = output_rate;
		reset();
		if (input_rate == output_rate) return;

		constexpr double pi = 3.14159265358979323846;
		const double cutoff = 0.94 * std::min(1.0, output_rate / input_rate);
		for (std::size_t phase = 0; phase < filters_.size(); ++phase) {
			const double fraction = static_cast<double>(phase) / filters_.size();
			double sum = 0;
			for (std::size_t k = 0; k < taps; ++k) {
				const double x = static_cast<double>(k) - (taps - 1) * 0.5 + fraction;
				const double window = 0.5 - 0.5 * std::cos(2 * pi * k / (taps - 1));
				const double value = std::abs(x) < 1e-10 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
				filters_[phase][k] = static_cast<float>(value * window);
				sum += value * window;
			}
			for (auto& coefficient : filters_[phase]) coefficient /= static_cast<float>(sum);
		}
	}

	void reset() {
		history_.fill(0);
		head_ = 0;
		position_ = output_rate_;
	}

	template <typename Read>
	bool next(Read&& read, float& output) {
		if (input_rate_ == output_rate_) return read(output);
		while (position_ >= output_rate_) {
			float sample;
			if (!read(sample)) return false;
			head_ = (head_ + taps - 1) % taps;
			history_[head_] = sample;
			position_ -= output_rate_;
		}
		const auto phase = static_cast<std::size_t>(position_ * filters_.size() / output_rate_);
		output = 0;
		for (std::size_t k = 0; k < taps; ++k)
			output += filters_[phase][k] * history_[(head_ + k) % taps];
		position_ += input_rate_;
		return true;
	}

private:
	static constexpr std::size_t taps = 48;
	std::array<std::array<float, taps>, 256> filters_{};
	std::array<float, taps> history_{};
	std::size_t head_{0};
	double input_rate_{48000}, output_rate_{48000}, position_{48000};
};

}
