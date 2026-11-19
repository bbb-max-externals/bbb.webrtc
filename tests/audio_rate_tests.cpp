#include "bbb/audio_rate_converter.hpp"
#include "bbb/opus_codec.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace bbb::webrtc;

std::vector<float> convert(const std::vector<float>& input, int from, int to) {
	audio_rate_converter converter;
	converter.setup(from, to);
	std::vector<float> output;
	size_t index = 0;
	// Exercise boundaries which do not divide either an Opus frame or a second.
	while (index < input.size()) {
		const auto end = std::min(index + 127, input.size());
		auto read = [&](float& sample) {
			if (index == end) return false;
			sample = input[index++];
			return true;
		};
		float sample;
		while (converter.next(read, sample)) output.push_back(sample);
	}
	return output;
}

bool check_tone(const std::vector<float>& samples, int rate) {
	if (samples.size() != static_cast<size_t>(rate)) return false;
	// Skip startup/codec delay. Count complete periods over half a second.
	int crossings = 0;
	for (int i = rate / 4; i < rate * 3 / 4; ++i)
		if (samples[i] <= 0 && samples[i + 1] > 0) ++crossings;
	return std::abs(crossings * 2 - 440) <= 2;
}

bool check_packet_durations() {
	for (int channels : {1, 2}) {
		opus_encoder encoder(48000, channels, 64000, 960);
		opus_decoder decoder(48000, channels, 960);
		std::vector<float> input(5760 * channels, 0.1f);
		std::vector<float> pcm(decoder.max_packet_frames() * channels);
		std::array<uint8_t, 4000> packet{};
		for (int frames : {960, 1920, 2880, 5760}) {
			int bytes = encoder.encode(input.data(), frames, packet.data(), packet.size());
			if (bytes <= 0 || decoder.decode(packet.data(), bytes, pcm.data(), decoder.max_packet_frames()) != frames)
				return false;
		}
	}
	return true;
}

int main() {
	if (!check_packet_durations()) {
		std::cerr << "Failed to decode a supported Opus packet duration\n";
		return 1;
	}
	for (int input_rate : {44100, 48000, 88200, 96000}) {
		std::vector<float> input(input_rate);
		for (int i = 0; i < input_rate; ++i)
			input[i] = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440 * i / input_rate));
		const auto codec_pcm = convert(input, input_rate, 48000);
		if (!check_tone(codec_pcm, 48000) || (input_rate == 48000 && codec_pcm != input)) return 1;

		opus_encoder encoder(48000, 1, 64000, 960);
		opus_decoder decoder(48000, 1, 960);
		std::vector<float> decoded(codec_pcm.size());
		std::array<uint8_t, 4000> packet{};
		for (size_t i = 0; i < codec_pcm.size(); i += 960) {
			const int bytes = encoder.encode(codec_pcm.data() + i, 960, packet.data(), packet.size());
			if (bytes <= 0 || decoder.decode(packet.data(), bytes, decoded.data() + i, 960) != 960) return 1;
		}
		for (int output_rate : {44100, 48000, 88200, 96000}) {
			if (!check_tone(convert(decoded, 48000, output_rate), output_rate)) {
				std::cerr << "Failed 440 Hz/one-second round trip at " << input_rate << " -> " << output_rate << '\n';
				return 1;
			}
		}
	}
	std::cout << "All 16 Opus sample-rate pairs preserved duration and pitch\n";
}
