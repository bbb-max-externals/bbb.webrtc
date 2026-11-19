#include "bbb/video_encoder.hpp"
#include <objbase.h>
#include <iostream>

namespace {
bool has_slice(const std::vector<uint8_t>& data) {
	for (size_t i = 0; i + 3 < data.size(); ++i) {
		if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
		auto type = data[i + 3] & 31;
		if (type == 1 || type == 5) return true;
	}
	return false;
}

bool encode_frame() {
	auto encoder = bbb::webrtc::create_video_encoder();
	if (!encoder->init(320, 240, 500000, 30)) {
		std::cerr << "Media Foundation H.264 initialization failed\n";
		return false;
	}
	bool emitted_slice = false;
	encoder->on_encoded([&](const bbb::webrtc::encoded_frame& frame) {
		emitted_slice |= has_slice(frame.data);
	});
	std::vector<uint8_t> rgba(320 * 240 * 4, 128);
	for (int i = 0; i < 60 && !emitted_slice; ++i) {
		if (!encoder->encode(rgba.data(), 320 * 4, i * 1000000ULL / 30)) {
			std::cerr << "Media Foundation rejected frame " << i << '\n';
			return false;
		}
	}
	if (!emitted_slice) std::cerr << "No Annex B video slice in encoder output\n";
	return emitted_slice;
}
}

int main() {
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
	const bool success = encode_frame();
	CoUninitialize();
	return success ? 0 : 1;
}
