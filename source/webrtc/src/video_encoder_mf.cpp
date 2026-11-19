#include "bbb/video_encoder.hpp"
#include "bbb/color_convert.hpp"

// NIL macro collides with Windows headers used by Media Foundation.
#pragma push_macro("NIL")
#undef NIL

#include <wrl/client.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <mferror.h>

#pragma pop_macro("NIL")

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bbb {
namespace webrtc {

namespace {

// MF uses 100-nanosecond time units. 1 second = 10^7.
constexpr int64_t kMFTimePerSecond = 10000000;

// MFVideoFormat_H264 and its optional sequence header use Annex B.
void read_sequence_header(IMFMediaType* type, std::vector<uint8_t>& header) {
	UINT32 size = 0;
	header.clear();
	if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || !size) return;
	header.resize(size);
	if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr)))
		header.clear();
}

bool is_keyframe(IMFSample *sample) {
	UINT32 clean_point = 0;
	return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) && clean_point;
}

HRESULT set_codec_property(ICodecAPI *codec_api, const GUID &property, ULONG value, VARTYPE type = VT_UI4) {
	VARIANT var;
	VariantInit(&var);
	var.vt = type;
	if (type == VT_BOOL) var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
	else var.ulVal = value;
	return codec_api->SetValue(&property, &var);
}

ComPtr<IMFSample> create_sample(const uint8_t *data, uint32_t size,
                                LONGLONG timestamp_100ns, LONGLONG duration_100ns) {
	ComPtr<IMFSample> sample;
	HRESULT hr = MFCreateSample(&sample);
	if(FAILED(hr)) {
		return nullptr;
	}

	ComPtr<IMFMediaBuffer> buffer;
	hr = MFCreateMemoryBuffer(size, &buffer);
	if(FAILED(hr)) {
		return nullptr;
	}

	BYTE *buffer_ptr = nullptr;
	hr = buffer->Lock(&buffer_ptr, nullptr, nullptr);
	if(FAILED(hr)) {
		return nullptr;
	}

	std::memcpy(buffer_ptr, data, size);
	buffer->Unlock();

	hr = buffer->SetCurrentLength(size);
	if(FAILED(hr)) {
		return nullptr;
	}

	sample->AddBuffer(buffer.Get());
	sample->SetSampleTime(timestamp_100ns);
	sample->SetSampleDuration(duration_100ns);

	return sample;
}

} // anonymous namespace

class mf_video_encoder : public video_encoder {
public:
	mf_video_encoder() = default;
	~mf_video_encoder() override;

	bool init(int width, int height, int bitrate_bps, int fps) override;
	void set_bitrate(int bitrate_bps) override;
	void request_keyframe() override;
	bool encode(const uint8_t *rgba_data, int stride, uint64_t timestamp_us) override;

private:
	bool create_transform();
	bool configure_input_type();
	bool configure_output_type();
	void shutdown();
	bool emit_sample(IMFSample* sample);

	ComPtr<IMFTransform> transform_;
	ComPtr<ICodecAPI> codec_api_;
	ComPtr<IMFMediaType> output_type_;

	int width_{0};
	int height_{0};
	int bitrate_bps_{0};
	int fps_{0};

	std::vector<uint8_t> sps_pps_;
	bool mf_started_{false};

	static constexpr DWORD kStream_id = 0;
};

mf_video_encoder::~mf_video_encoder() {
	shutdown();
}

bool mf_video_encoder::init(int width, int height, int bitrate_bps, int fps) {
	shutdown();
	if (width <= 0 || height <= 0 || width % 2 || height % 2 || bitrate_bps <= 0 || fps <= 0)
		return false;
	width_ = width;
	height_ = height;
	bitrate_bps_ = bitrate_bps;
	fps_ = fps;
	sps_pps_.clear();

	HRESULT hr = MFStartup(MF_VERSION);
	if(FAILED(hr)) {
		return false;
	}

	mf_started_ = true;
	if (create_transform()) return true;
	shutdown();
	return false;
}

bool mf_video_encoder::create_transform() {
	MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
	MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
	IMFActivate** candidates = nullptr;
	UINT32 count = 0;
	if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
	                     MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
	                     &input, &output, &candidates, &count))) return false;

	// This pump is synchronous. Try each matching software transform until
	// negotiation and streaming both succeed.
	bool ready = false;
	for (UINT32 i = 0; i < count; ++i) {
		if (!ready && SUCCEEDED(candidates[i]->ActivateObject(IID_PPV_ARGS(&transform_)))) {
			transform_.As(&codec_api_);
			if (codec_api_)
				set_codec_property(codec_api_.Get(), CODECAPI_AVLowLatencyMode, TRUE, VT_BOOL);
			ready = configure_output_type() && configure_input_type() &&
				SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) &&
				SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
			if (!ready) {
				codec_api_.Reset();
				output_type_.Reset();
				transform_.Reset();
				candidates[i]->ShutdownObject();
			}
		}
		candidates[i]->Release();
	}
	CoTaskMemFree(candidates);
	return ready;
}

bool mf_video_encoder::configure_input_type() {
	ComPtr<IMFMediaType> input_type;
	HRESULT hr = MFCreateMediaType(&input_type);
	if(FAILED(hr)) {
		return false;
	}

	hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if(FAILED(hr)) return false;

	hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	if(FAILED(hr)) return false;

	hr = MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE,
	                        static_cast<UINT32>(width_),
	                        static_cast<UINT32>(height_));
	if(FAILED(hr)) return false;

	hr = MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE,
	                         static_cast<UINT32>(fps_), 1);
	if(FAILED(hr)) return false;

	hr = MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if(FAILED(hr)) return false;

	hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE,
	                            MFVideoInterlace_Progressive);
	if(FAILED(hr)) return false;

	hr = transform_->SetInputType(kStream_id, input_type.Get(), 0);
	return SUCCEEDED(hr);
}

bool mf_video_encoder::configure_output_type() {
	ComPtr<IMFMediaType> output_type;
	HRESULT hr = MFCreateMediaType(&output_type);
	if(FAILED(hr)) {
		return false;
	}

	hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if(FAILED(hr)) return false;

	hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	if(FAILED(hr)) return false;

	hr = MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE,
	                        static_cast<UINT32>(width_),
	                        static_cast<UINT32>(height_));
	if(FAILED(hr)) return false;

	if (FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps_)) ||
	    FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps_, 1)) ||
	    FAILED(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
	    FAILED(output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base))) return false;
	hr = transform_->SetOutputType(kStream_id, output_type.Get(), 0);
	if(FAILED(hr)) return false;

	hr = transform_->GetOutputCurrentType(kStream_id, &output_type_);
	if(FAILED(hr)) return false;

	read_sequence_header(output_type_.Get(), sps_pps_);

	return true;
}

void mf_video_encoder::set_bitrate(int bitrate_bps) {
	bitrate_bps_ = bitrate_bps;
	if(codec_api_) {
		set_codec_property(codec_api_.Get(),
		                  CODECAPI_AVEncCommonMeanBitRate,
		                  static_cast<LONG>(bitrate_bps));
	}
}

void mf_video_encoder::request_keyframe() {
	if(codec_api_) {
		set_codec_property(codec_api_.Get(),
		                  CODECAPI_AVEncVideoForceKeyFrame, TRUE);
	}
}

bool mf_video_encoder::encode(const uint8_t *rgba_data, int stride,
                               uint64_t timestamp_us) {
	if(!transform_) {
		return false;
	}

	std::vector<uint8_t> nv12;
	rgba_to_nv12(rgba_data, width_, height_, stride, nv12);

	int nv12_stride = width_;
	int nv12_frame_size = nv12_stride * height_ + nv12_stride * (height_ / 2);

	LONGLONG timestamp_100ns = static_cast<LONGLONG>(timestamp_us) * 10;

	ComPtr<IMFSample> input_sample = create_sample(
	    nv12.data(), static_cast<uint32_t>(nv12_frame_size), timestamp_100ns, kMFTimePerSecond / fps_);
	if(!input_sample) {
		return false;
	}

	HRESULT hr = transform_->ProcessInput(kStream_id, input_sample.Get(), 0);
	if(FAILED(hr)) {
		return false;
	}

	for (;;) {
		MFT_OUTPUT_STREAM_INFO info{};
		if (FAILED(transform_->GetOutputStreamInfo(kStream_id, &info))) return false;
		MFT_OUTPUT_DATA_BUFFER output{};
		ComPtr<IMFSample> sample;
		if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(MFCreateSample(&sample)) ||
			    FAILED(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer)) ||
			    FAILED(sample->AddBuffer(buffer.Get()))) return false;
			output.pSample = sample.Get();
		}

		DWORD status = 0;
		hr = transform_->ProcessOutput(0, 1, &output, &status);
		if (output.pEvents) output.pEvents->Release();
		if (!sample) sample.Attach(output.pSample);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			if (!configure_output_type()) return false;
			continue;
		}
		if (FAILED(hr) || !sample || !emit_sample(sample.Get())) return false;
	}
}

bool mf_video_encoder::emit_sample(IMFSample* sample) {
	ComPtr<IMFMediaBuffer> buffer;
	if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
	BYTE* data = nullptr;
	DWORD length = 0;
	if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
	encoded_frame frame;
	frame.is_keyframe = is_keyframe(sample);
	LONGLONG time = 0;
	sample->GetSampleTime(&time);
	frame.timestamp_us = static_cast<uint64_t>(time / 10);
	if (frame.is_keyframe) frame.data = sps_pps_;
	if (length) frame.data.insert(frame.data.end(), data, data + length);
	buffer->Unlock();
	if (length && encoded_callback_) encoded_callback_(frame);
	return true;
}

void mf_video_encoder::shutdown() {
	if (transform_) {
		transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
	}
	codec_api_.Reset();
	output_type_.Reset();
	transform_.Reset();
	if (mf_started_) {
		MFShutdown();
		mf_started_ = false;
	}
}

std::unique_ptr<video_encoder> create_video_encoder() {
	return std::make_unique<mf_video_encoder>();
}

} // namespace webrtc
} // namespace bbb
