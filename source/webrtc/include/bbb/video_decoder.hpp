#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bbb {
namespace webrtc {

struct decoded_frame {
	std::vector<uint8_t> data; // RGBA interleaved, 4 bytes per pixel, top-left origin
	int width{0};
	int height{0};
	uint64_t timestamp_us{0};
};

class video_decoder {
public:
	using decoded_callback = std::function<void(const decoded_frame &)>;

	virtual ~video_decoder() = default;

	virtual bool init() = 0;

	// nal_data: Annex-B H.264 NAL units (start code prefixed)
	virtual bool decode(const uint8_t *nal_data, size_t nal_size, uint64_t timestamp_us) = 0;

	void on_decoded(decoded_callback cb) { decoded_callback_ = std::move(cb); }

protected:
	decoded_callback decoded_callback_;
};

// Platform-specific factory (implemented in video_decoder_videotoolbox.mm / video_decoder_mf.cpp)
std::unique_ptr<video_decoder> create_video_decoder();

} // namespace webrtc
} // namespace bbb
