#include "bbb/opus_codec.hpp"

#include <opus.h>

#include <stdexcept>

namespace bbb {
namespace webrtc {

// ---- encoder ----

struct opus_encoder::impl {
	OpusEncoder *enc{nullptr};
	int sample_rate;
	int channels;
	int frame_size;

	impl(int sample_rate, int channels, int bitrate, int frame_size)
		: sample_rate(sample_rate)
		, channels(channels)
		, frame_size(frame_size) {
		int error = 0;
		enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &error);
		if(error != OPUS_OK || !enc) {
			throw std::runtime_error("opus_encoder_create failed");
		}
		opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
	}

	~impl() {
		if(enc) opus_encoder_destroy(enc);
	}
};

opus_encoder::opus_encoder(int sample_rate, int channels, int bitrate, int frame_size)
	: impl_(std::make_unique<impl>(sample_rate, channels, bitrate, frame_size)) {}

opus_encoder::~opus_encoder() = default;

int opus_encoder::encode(const float *pcm, int frame_size, std::uint8_t *output, int max_output_bytes) {
	return opus_encode_float(impl_->enc, pcm, frame_size, output, max_output_bytes);
}

void opus_encoder::set_bitrate(int bitrate) {
	if(impl_->enc) {
		opus_encoder_ctl(impl_->enc, OPUS_SET_BITRATE(bitrate));
	}
}

int opus_encoder::get_bitrate() const {
	opus_int32 bitrate = 0;
	if(impl_->enc) {
		opus_encoder_ctl(impl_->enc, OPUS_GET_BITRATE(&bitrate));
	}
	return static_cast<int>(bitrate);
}

// ---- decoder ----

struct opus_decoder::impl {
	OpusDecoder *dec{nullptr};
	int sample_rate;
	int channels;

	impl(int sample_rate, int channels)
		: sample_rate(sample_rate)
		, channels(channels) {
		int error = 0;
		dec = opus_decoder_create(sample_rate, channels, &error);
		if(error != OPUS_OK || !dec) {
			throw std::runtime_error("opus_decoder_create failed");
		}
	}

	~impl() {
		if(dec) opus_decoder_destroy(dec);
	}
};

opus_decoder::opus_decoder(int sample_rate, int channels, int frame_size)
	: frame_size_(frame_size)
	, impl_(std::make_unique<impl>(sample_rate, channels)) {}

opus_decoder::~opus_decoder() = default;

int opus_decoder::decode(const std::uint8_t *data, int size, float *pcm, int max_frames) {
	return opus_decode_float(impl_->dec, data, size, pcm, max_frames, 0);
}

} // namespace webrtc
} // namespace bbb
