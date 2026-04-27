#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bbb {
namespace webrtc {

struct encoded_frame {
	std::vector<uint8_t> data; // Annex-B H.264 NAL units (start code prefixed)
	bool is_keyframe{false};
	uint64_t timestamp_us{0};
};

class video_encoder {
public:
	using encoded_callback = std::function<void(const encoded_frame &)>;

	virtual ~video_encoder() = default;

	virtual bool init(int width, int height, int bitrate_bps, int fps) = 0;
	virtual void set_bitrate(int bitrate_bps) = 0;
	virtual void request_keyframe() = 0;

	// rgba_data: RGBA interleaved, 4 bytes per pixel, top-left origin
	// stride: bytes per row (typically width * 4)
	// Returns true if the frame was accepted for encoding.
	virtual bool encode(const uint8_t *rgba_data, int stride, uint64_t timestamp_us) = 0;

	void on_encoded(encoded_callback cb) { encoded_callback_ = std::move(cb); }

protected:
	encoded_callback encoded_callback_;
};

// Platform-specific factory (implemented in video_encoder_videotoolbox.mm / video_encoder_mf.cpp)
std::unique_ptr<video_encoder> create_video_encoder();

} // namespace webrtc
} // namespace bbb
