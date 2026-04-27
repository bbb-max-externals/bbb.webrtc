#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bbb {
namespace webrtc {

class opus_encoder {
public:
	opus_encoder(int sample_rate = 48000, int channels = 1, int bitrate = 64000, int frame_size = 960);
	~opus_encoder();

	opus_encoder(const opus_encoder &) = delete;
	opus_encoder &operator=(const opus_encoder &) = delete;

	// encode interleaved float PCM to opus packet
	// returns encoded size, or -1 on error
	int encode(const float *pcm, int frame_size, std::uint8_t *output, int max_output_bytes);

	void set_bitrate(int bitrate);
	int get_bitrate() const;

private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

class opus_decoder {
public:
	opus_decoder(int sample_rate = 48000, int channels = 1, int frame_size = 960);
	~opus_decoder();

	opus_decoder(const opus_decoder &) = delete;
	opus_decoder &operator=(const opus_decoder &) = delete;

	// decode opus packet to interleaved float PCM
	// returns number of decoded frames, or -1 on error
	int decode(const std::uint8_t *data, int size, float *pcm, int max_frames);

	int get_frame_size() const { return frame_size_; }

private:
	int frame_size_;
	struct impl;
	std::unique_ptr<impl> impl_;
};

} // namespace webrtc
} // namespace bbb
