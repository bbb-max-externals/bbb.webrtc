#include "bbb/video_decoder.hpp"
#include "bbb/color_convert.hpp"

#pragma push_macro("NIL")
#undef NIL

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#pragma pop_macro("NIL")

#include <cstring>
#include <vector>

namespace bbb {
namespace webrtc {

class videotoolbox_decoder : public video_decoder {
public:
	videotoolbox_decoder() = default;
	~videotoolbox_decoder() override;

	bool init() override;
	bool decode(const uint8_t *nal_data, size_t nal_size, uint64_t timestamp_us) override;

private:
	void destroy_session();
	bool create_session();

	void decompression_output_callback(OSStatus status,
	                                   VTDecodeInfoFlags info_flags,
	                                   CVImageBufferRef image_buffer,
	                                   CMTime presentation_timestamp,
	                                   CMTime presentation_duration);

	static void decompression_output_callback_static(void *decompression_output_refcon,
	                                                 void *source_frame_refcon,
	                                                 OSStatus status,
	                                                 VTDecodeInfoFlags info_flags,
	                                                 CVImageBufferRef image_buffer,
	                                                 CMTime presentation_timestamp,
	                                                 CMTime presentation_duration);

	VTDecompressionSessionRef session_{nullptr};
	CMVideoFormatDescriptionRef format_desc_{nullptr};

	std::vector<uint8_t> last_sps_;
	std::vector<uint8_t> last_pps_;
	int width_{0};
	int height_{0};
	uint64_t current_timestamp_us_{0};
};

videotoolbox_decoder::~videotoolbox_decoder() {
	destroy_session();
}

void videotoolbox_decoder::destroy_session() {
	if(session_) {
		VTDecompressionSessionInvalidate(session_);
		CFRelease(session_);
		session_ = nullptr;
	}
	if(format_desc_) {
		CFRelease(format_desc_);
		format_desc_ = nullptr;
	}
}

bool videotoolbox_decoder::init() {
	return true;
}

bool videotoolbox_decoder::create_session() {
	if(!format_desc_) {
		return false;
	}

	destroy_session();

	CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format_desc_);
	width_ = dims.width;
	height_ = dims.height;

	VTDecompressionOutputCallbackRecord callback;
	callback.decompressionOutputCallback = decompression_output_callback_static;
	callback.decompressionOutputRefCon = this;

	NSDictionary *destination_attrs = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES
	};

	OSStatus status = VTDecompressionSessionCreate(nullptr,
		format_desc_,
		nullptr,
		(__bridge CFDictionaryRef)destination_attrs,
		&callback,
		&session_);

	if(status != noErr) {
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTDecompressionPropertyKey_RealTime,
		kCFBooleanTrue);

	if(status != noErr) {
		destroy_session();
		return false;
	}

	return true;
}

static std::vector<std::pair<const uint8_t *, size_t>> find_nal_units(const uint8_t *data, size_t size) {
	std::vector<std::pair<const uint8_t *, size_t>> units;

	size_t i = 0;
	while(i + 3 <= size) {
		bool is_start_code = false;
		int start_code_len = 0;

		if(i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
			is_start_code = true;
			start_code_len = 4;
		} else if(data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
			is_start_code = true;
			start_code_len = 3;
		}

		if(is_start_code) {
			const uint8_t *nal_start = data + i + start_code_len;
			const uint8_t *nal_end = data + size;
			size_t j = i + start_code_len;
			while(j + 3 <= size) {
				bool found = false;
				if(j + 4 <= size && data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x00 && data[j + 3] == 0x01) {
					nal_end = data + j;
					found = true;
				} else if(data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01) {
					nal_end = data + j;
					found = true;
				}
				if(found) {
					break;
				}
				++j;
			}

			size_t len = nal_end - nal_start;
			if(len > 0) {
				units.push_back({nal_start, len});
			}

			i = nal_end - data;
		} else {
			++i;
		}
	}

	return units;
}

static void append_avcc_nalu(std::vector<uint8_t> &buffer, const uint8_t *nalu, size_t len) {
	buffer.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>(len & 0xFF));
	buffer.insert(buffer.end(), nalu, nalu + len);
}

bool videotoolbox_decoder::decode(const uint8_t *nal_data, size_t nal_size, uint64_t timestamp_us) {
	current_timestamp_us_ = timestamp_us;

	auto nal_units = find_nal_units(nal_data, nal_size);

	const uint8_t *sps_ptr = nullptr;
	size_t sps_len = 0;
	const uint8_t *pps_ptr = nullptr;
	size_t pps_len = 0;

	for(const auto &unit : nal_units) {
		uint8_t nalu_type = (unit.first[0] & 0x1F);
		if(nalu_type == 7 && !sps_ptr) {
			sps_ptr = unit.first;
			sps_len = unit.second;
		} else if(nalu_type == 8 && !pps_ptr) {
			pps_ptr = unit.first;
			pps_len = unit.second;
		}
	}

	bool need_new_session = false;

	if(sps_ptr && sps_len > 0 && pps_ptr && pps_len > 0) {
		if(last_sps_.size() != sps_len || last_pps_.size() != pps_len ||
		   std::memcmp(last_sps_.data(), sps_ptr, sps_len) != 0 ||
		   std::memcmp(last_pps_.data(), pps_ptr, pps_len) != 0) {

			last_sps_.assign(sps_ptr, sps_ptr + sps_len);
			last_pps_.assign(pps_ptr, pps_ptr + pps_len);

			const uint8_t *param_sets[] = {sps_ptr, pps_ptr};
			size_t param_set_sizes[] = {sps_len, pps_len};
			OSStatus s = CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr,
				2, param_sets, param_set_sizes,
				4, &format_desc_);

			if(s != noErr || !format_desc_) {
				return false;
			}

			need_new_session = true;
		}
	}

	if(need_new_session) {
		if(!create_session()) {
			return false;
		}
	}

	if(!session_ || !format_desc_) {
		return true;
	}

	std::vector<uint8_t> avcc_buffer;
	for(const auto &unit : nal_units) {
		uint8_t nalu_type = (unit.first[0] & 0x1F);
		if(nalu_type != 7 && nalu_type != 8) {
			append_avcc_nalu(avcc_buffer, unit.first, unit.second);
		}
	}

	if(avcc_buffer.empty()) {
		return true;
	}

	CMBlockBufferRef block_buffer = nullptr;
	OSStatus s = CMBlockBufferCreateWithMemoryBlock(nullptr,
		avcc_buffer.data(), avcc_buffer.size(),
		nullptr, nullptr, 0, avcc_buffer.size(),
		0, &block_buffer);

	if(s != noErr || !block_buffer) {
		return false;
	}

	CMTime pts = CMTimeMake(timestamp_us, 1000000);
	CMTime duration = CMTimeMake(1, 30);

	CMSampleTimingInfo timing;
	timing.presentationTimeStamp = pts;
	timing.decodeTimeStamp = kCMTimeInvalid;
	timing.duration = duration;

	size_t sample_size = avcc_buffer.size();
	CMSampleBufferRef sample_buffer = nullptr;
	s = CMSampleBufferCreate(nullptr,
		block_buffer, true, nullptr, nullptr,
		format_desc_,
		1, 1, &timing,
		1, &sample_size,
		&sample_buffer);

	if(s != noErr || !sample_buffer) {
		CFRelease(block_buffer);
		return false;
	}

	VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
	VTDecodeInfoFlags info_flags = 0;

	s = VTDecompressionSessionDecodeFrame(session_,
		sample_buffer,
		flags,
		nullptr, &info_flags);

	CFRelease(sample_buffer);
	CFRelease(block_buffer);

	if(s != noErr && s != kVTInvalidSessionErr) {
		return false;
	}

	if(s == kVTInvalidSessionErr) {
		return false;
	}

	return true;
}

void videotoolbox_decoder::decompression_output_callback_static(void *decompression_output_refcon,
                                                                 void *source_frame_refcon,
                                                                 OSStatus status,
                                                                 VTDecodeInfoFlags info_flags,
                                                                 CVImageBufferRef image_buffer,
                                                                 CMTime presentation_timestamp,
                                                                 CMTime presentation_duration) {
	auto *decoder = static_cast<videotoolbox_decoder *>(decompression_output_refcon);
	(void)source_frame_refcon;
	if(decoder) {
		decoder->decompression_output_callback(status, info_flags,
			image_buffer, presentation_timestamp, presentation_duration);
	}
}

void videotoolbox_decoder::decompression_output_callback(OSStatus status,
                                                          VTDecodeInfoFlags info_flags,
                                                          CVImageBufferRef image_buffer,
                                                          CMTime presentation_timestamp,
                                                          CMTime presentation_duration) {
	(void)info_flags;
	(void)presentation_timestamp;
	(void)presentation_duration;

	if(status != noErr || !image_buffer) {
		return;
	}

	if(!decoded_callback_) {
		return;
	}

	CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)image_buffer;
	CVPixelBufferLockBaseAddress(pixel_buffer, 0);

	int buffer_width = (int)CVPixelBufferGetWidth(pixel_buffer);
	int buffer_height = (int)CVPixelBufferGetHeight(pixel_buffer);

	if(buffer_width != width_ || buffer_height != height_) {
		width_ = buffer_width;
		height_ = buffer_height;
	}

	const uint8_t *y_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
	const uint8_t *uv_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));

	int y_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
	int uv_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);

	std::vector<uint8_t> nv12;
	nv12.resize(width_ * height_ * 3 / 2);

	for(int row = 0; row < height_; ++row) {
		std::memcpy(nv12.data() + row * width_, y_plane + row * y_stride, width_);
	}

	for(int row = 0; row < height_ / 2; ++row) {
		std::memcpy(nv12.data() + width_ * height_ + row * width_,
		            uv_plane + row * uv_stride,
		            width_);
	}

	CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

	decoded_frame frame;
	frame.width = width_;
	frame.height = height_;
	frame.timestamp_us = current_timestamp_us_;
	frame.data.resize(width_ * height_ * 4);

	nv12_to_rgba(nv12.data(), width_, height_, frame.data.data(), width_ * 4);

	decoded_callback_(frame);
}

std::unique_ptr<video_decoder> create_video_decoder() {
	return std::make_unique<videotoolbox_decoder>();
}

} // namespace webrtc
} // namespace bbb
