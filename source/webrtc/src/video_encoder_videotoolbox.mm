#include "bbb/video_encoder.hpp"
#include "bbb/color_convert.hpp"

#pragma push_macro("NIL")
#undef NIL

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#pragma pop_macro("NIL")

#include <cstring>

namespace bbb {
namespace webrtc {

static const uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};

class videotoolbox_encoder : public video_encoder {
public:
	videotoolbox_encoder() = default;
	~videotoolbox_encoder() override;

	bool init(int width, int height, int bitrate_bps, int fps) override;
	void set_bitrate(int bitrate_bps) override;
	void request_keyframe() override;
	bool encode(const uint8_t *rgba_data, int stride, uint64_t timestamp_us) override;

private:
	void destroy_session();
	void compression_output_callback(OSStatus status,
	                                 VTEncodeInfoFlags info_flags,
	                                 CMSampleBufferRef sample_buffer);

	static void compression_output_callback_static(void *output_callback_refcon,
	                                               void *source_frame_refcon,
	                                               OSStatus status,
	                                               VTEncodeInfoFlags info_flags,
	                                               CMSampleBufferRef sample_buffer);

	VTCompressionSessionRef session_{nullptr};
	int width_{0};
	int height_{0};
	int fps_{0};
	int bitrate_bps_{0};
	bool force_keyframe_{false};
	std::vector<uint8_t> sps_pps_;
	std::vector<uint8_t> nv12_buffer_;
};

videotoolbox_encoder::~videotoolbox_encoder() {
	destroy_session();
}

void videotoolbox_encoder::destroy_session() {
	if(session_) {
		VTCompressionSessionInvalidate(session_);
		CFRelease(session_);
		session_ = nullptr;
	}
	sps_pps_.clear();
}

bool videotoolbox_encoder::init(int width, int height, int bitrate_bps, int fps) {
	destroy_session();

	width_ = width;
	height_ = height;
	fps_ = fps;
	bitrate_bps_ = bitrate_bps;
	force_keyframe_ = false;

	nv12_buffer_.resize(width * height * 3 / 2);

	OSStatus status = VTCompressionSessionCreate(
		nullptr,
		width, height,
		kCMVideoCodecType_H264,
		nullptr, nullptr, nullptr,
		compression_output_callback_static,
		this, &session_);

	if(status != noErr) {
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_RealTime,
		kCFBooleanTrue);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_ProfileLevel,
		kVTProfileLevel_H264_Baseline_AutoLevel);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_AllowFrameReordering,
		kCFBooleanFalse);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	CFNumberRef max_delay = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fps);
	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_MaxFrameDelayCount,
		max_delay);
	CFRelease(max_delay);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
		kCFBooleanTrue);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_AverageBitRate,
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate_bps));
	if(status != noErr) {
		destroy_session();
		return false;
	}

	int bytes_per_second = bitrate_bps / 8;
	int one_second_limit = bytes_per_second;
	CFNumberRef limits[] = {
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &one_second_limit),
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &one_second_limit)
	};
	CFArrayRef data_rate_limits = CFArrayCreate(nullptr, (const void **)limits, 2, &kCFTypeArrayCallBacks);
	CFRelease(limits[0]);
	CFRelease(limits[1]);

	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_DataRateLimits,
		data_rate_limits);
	CFRelease(data_rate_limits);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	int expected_frame_duration = fps > 0 ? (600 / fps) : 20;
	CFNumberRef frame_duration = CFNumberCreate(nullptr, kCFNumberSInt32Type, &expected_frame_duration);
	status = VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_ExpectedFrameRate,
		frame_duration);
	CFRelease(frame_duration);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	status = VTCompressionSessionPrepareToEncodeFrames(session_);
	if(status != noErr) {
		destroy_session();
		return false;
	}

	return true;
}

void videotoolbox_encoder::set_bitrate(int bitrate_bps) {
	bitrate_bps_ = bitrate_bps;
	if(!session_) {
		return;
	}

	VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_AverageBitRate,
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate_bps));

	int bytes_per_second = bitrate_bps / 8;
	CFNumberRef limits[] = {
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &bytes_per_second),
		CFNumberCreate(nullptr, kCFNumberSInt32Type, &bytes_per_second)
	};
	CFArrayRef data_rate_limits = CFArrayCreate(nullptr, (const void **)limits, 2, &kCFTypeArrayCallBacks);
	CFRelease(limits[0]);
	CFRelease(limits[1]);

	VTSessionSetProperty(session_,
		kVTCompressionPropertyKey_DataRateLimits,
		data_rate_limits);
	CFRelease(data_rate_limits);
}

void videotoolbox_encoder::request_keyframe() {
	force_keyframe_ = true;
}

bool videotoolbox_encoder::encode(const uint8_t *rgba_data, int stride, uint64_t timestamp_us) {
	if(!session_) {
		return false;
	}

	rgba_to_nv12(rgba_data, width_, height_, stride, nv12_buffer_);

	int y_size = width_ * height_;
	int uv_size = y_size / 2;

	CVPixelBufferRef pixel_buffer = nullptr;

	NSDictionary *attrs = @{
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES
	};

	CVReturn cv_ret = CVPixelBufferCreate(nullptr,
		width_, height_,
		kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
		(__bridge CFDictionaryRef)attrs,
		&pixel_buffer);

	if(cv_ret != kCVReturnSuccess || !pixel_buffer) {
		return false;
	}

	CVPixelBufferLockBaseAddress(pixel_buffer, 0);
	uint8_t *y_plane = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
	uint8_t *uv_plane = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));

	int y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
	int uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);

	std::memcpy(y_plane, nv12_buffer_.data(), y_size);

	int uv_width = width_;
	for(int row = 0; row < height_ / 2; ++row) {
		std::memcpy(uv_plane + row * uv_stride,
		            nv12_buffer_.data() + y_size + row * uv_width,
		            uv_width);
	}

	CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

	CMTime presentation_time = CMTimeMake(timestamp_us, 1000000);
	CMTime duration = CMTimeMake(1, fps_ > 0 ? fps_ : 30);

	CFDictionaryRef frame_properties = nullptr;
	if(force_keyframe_) {
		const void *keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
		const void *values[] = {kCFBooleanTrue};
		frame_properties = CFDictionaryCreate(nullptr, keys, values, 1,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		force_keyframe_ = false;
	}

	OSStatus status = VTCompressionSessionEncodeFrame(session_,
		pixel_buffer,
		presentation_time,
		duration,
		frame_properties,
		nullptr, nullptr);

	if(frame_properties) {
		CFRelease(frame_properties);
	}
	CVPixelBufferRelease(pixel_buffer);

	if(status != noErr) {
		return false;
	}

	return true;
}

void videotoolbox_encoder::compression_output_callback_static(void *output_callback_refcon,
                                                               void *source_frame_refcon,
                                                               OSStatus status,
                                                               VTEncodeInfoFlags info_flags,
                                                               CMSampleBufferRef sample_buffer) {
	(void)source_frame_refcon;
	auto *encoder = static_cast<videotoolbox_encoder *>(output_callback_refcon);
	if(encoder) {
		encoder->compression_output_callback(status, info_flags, sample_buffer);
	}
}

void videotoolbox_encoder::compression_output_callback(OSStatus status,
                                                        VTEncodeInfoFlags info_flags,
                                                        CMSampleBufferRef sample_buffer) {
	(void)info_flags;

	if(status != noErr || !sample_buffer) {
		return;
	}

	if(!encoded_callback_) {
		return;
	}

	bool is_keyframe = false;
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, false);
	if(attachments && CFArrayGetCount(attachments) > 0) {
		CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
		CFBooleanRef depends = (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_DependsOnOthers);
		if(depends) {
			is_keyframe = !CFBooleanGetValue(depends);
		} else {
			is_keyframe = true;
		}
	}

	CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buffer);
	uint64_t timestamp_us = CMTIME_IS_VALID(pts)
		? static_cast<uint64_t>(CMTimeGetSeconds(pts) * 1000000.0)
		: 0;

	if(is_keyframe) {
		CMFormatDescriptionRef format_desc = CMSampleBufferGetFormatDescription(sample_buffer);
		if(format_desc) {
			const uint8_t *sps_data = nullptr;
			size_t sps_size = 0;
			const uint8_t *pps_data = nullptr;
			size_t pps_size = 0;

			OSStatus s = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format_desc, 0,
				&sps_data, &sps_size, nullptr, nullptr);
			if(s == noErr && sps_data && sps_size > 0) {
				sps_pps_.clear();
				sps_pps_.insert(sps_pps_.end(), kStartCode, kStartCode + 4);
				sps_pps_.insert(sps_pps_.end(), sps_data, sps_data + sps_size);
			}

			s = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format_desc, 1,
				&pps_data, &pps_size, nullptr, nullptr);
			if(s == noErr && pps_data && pps_size > 0) {
				sps_pps_.insert(sps_pps_.end(), kStartCode, kStartCode + 4);
				sps_pps_.insert(sps_pps_.end(), pps_data, pps_data + pps_size);
			}
		}
	}

	CMBlockBufferRef block_buffer = CMSampleBufferGetDataBuffer(sample_buffer);
	if(!block_buffer) {
		return;
	}

	size_t total_length = CMBlockBufferGetDataLength(block_buffer);
	if(total_length == 0) {
		return;
	}

	char *data_ptr = nullptr;
	OSStatus s = CMBlockBufferGetDataPointer(block_buffer, 0, nullptr, nullptr, &data_ptr);
	if(s != noErr || !data_ptr) {
		return;
	}

	std::vector<uint8_t> annex_b;

	if(is_keyframe && !sps_pps_.empty()) {
		annex_b = sps_pps_;
	}

	size_t offset = 0;
	while(offset + 4 <= total_length) {
		uint32_t nal_length = (uint32_t(data_ptr[offset]) << 24) |
		                      (uint32_t(data_ptr[offset + 1]) << 16) |
		                      (uint32_t(data_ptr[offset + 2]) << 8) |
		                      uint32_t(data_ptr[offset + 3]);

		if(nal_length == 0 || offset + 4 + nal_length > total_length) {
			break;
		}

		annex_b.insert(annex_b.end(), kStartCode, kStartCode + 4);
		annex_b.insert(annex_b.end(),
		               reinterpret_cast<uint8_t *>(data_ptr + offset + 4),
		               reinterpret_cast<uint8_t *>(data_ptr + offset + 4 + nal_length));

		offset += 4 + nal_length;
	}

	if(annex_b.empty()) {
		return;
	}

	encoded_frame frame;
	frame.data = std::move(annex_b);
	frame.is_keyframe = is_keyframe;
	frame.timestamp_us = timestamp_us;

	encoded_callback_(frame);
}

std::unique_ptr<video_encoder> create_video_encoder() {
	return std::make_unique<videotoolbox_encoder>();
}

} // namespace webrtc
} // namespace bbb
