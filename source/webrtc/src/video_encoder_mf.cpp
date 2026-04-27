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

static const uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// AVCC uses length-prefixed NAL units; Annex-B uses start code (00 00 00 01).
void avcc_to_annex_b(const uint8_t *data, size_t size, uint32_t nal_length_size,
                     std::vector<uint8_t> &out) {
	size_t pos = 0;
	while(pos + nal_length_size <= size) {
		uint32_t nal_length = 0;
		for(uint32_t i = 0; i < nal_length_size; i++) {
			nal_length = (nal_length << 8) | data[pos + i];
		}
		pos += nal_length_size;

		if(pos + nal_length > size) {
			break;
		}

		out.insert(out.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
		out.insert(out.end(), data + pos, data + pos + nal_length);
		pos += nal_length;
	}
}

// MF_MT_MPEG_SEQUENCE_HEADER contains AVCC-format header:
//   byte[0]: version (1)
//   byte[1]: profile
//   byte[2]: compatibility
//   byte[3]: level
//   byte[4]: NALU length size - 1 (lower 2 bits)
//   byte[5]: num_sps (lower 5 bits)
//   byte[6..]: 16-bit big-endian SPS length, SPS data, ...
//              then num_pps (8-bit), 16-bit big-endian PPS length, PPS data
bool extract_sps_pps_from_sequence_header(IMFMediaType *media_type,
                                          std::vector<uint8_t> &sps_pps) {
	UINT32 header_size = 0;
	UINT8 *header_data = nullptr;

	HRESULT hr = media_type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, nullptr, 0,
	                                 &header_size);
	if(FAILED(hr) || header_size == 0) {
		return false;
	}

	header_data = new UINT8[header_size];
	hr = media_type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header_data,
	                         header_size, &header_size);
	if(FAILED(hr)) {
		delete[] header_data;
		return false;
	}

	if(header_size < 5) {
		delete[] header_data;
		return false;
	}

	uint32_t nal_length_size = (header_data[4] & 0x03) + 1;
	size_t pos = 5;

	if(pos >= header_size) {
		delete[] header_data;
		return false;
	}
	uint8_t num_sps = header_data[pos] & 0x1F;
	pos++;

	for(uint8_t i = 0; i < num_sps && pos < header_size; i++) {
		if(pos + 2 > header_size) break;
		uint16_t sps_length = (header_data[pos] << 8) | header_data[pos + 1];
		pos += 2;

		if(pos + sps_length > header_size) break;

		sps_pps.insert(sps_pps.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
		sps_pps.insert(sps_pps.end(), header_data + pos, header_data + pos + sps_length);
		pos += sps_length;
	}

	if(pos >= header_size) {
		delete[] header_data;
		return !sps_pps.empty();
	}
	uint8_t num_pps = header_data[pos];
	pos++;

	for(uint8_t i = 0; i < num_pps && pos < header_size; i++) {
		if(pos + 2 > header_size) break;
		uint16_t pps_length = (header_data[pos] << 8) | header_data[pos + 1];
		pos += 2;

		if(pos + pps_length > header_size) break;

		sps_pps.insert(sps_pps.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
		sps_pps.insert(sps_pps.end(), header_data + pos, header_data + pos + pps_length);
		pos += pps_length;
	}

	delete[] header_data;
	return !sps_pps.empty();
}

bool is_keyframe(IMFSample *sample) {
	BOOL clean_point = FALSE;
	HRESULT hr = sample->GetUINT32(MFSampleExtension_CleanPoint,
	                               reinterpret_cast<UINT32 *>(&clean_point));
	if(SUCCEEDED(hr) && clean_point) {
		return true;
	}
	hr = sample->GetUINT32(MFSampleExtension_Discontinuity,
	                       reinterpret_cast<UINT32 *>(&clean_point));
	return SUCCEEDED(hr) && clean_point;
}

HRESULT set_codec_property(ICodecAPI *codec_api, const GUID &property, LONG value) {
	VARIANT var;
	VariantInit(&var);
	var.vt = VT_I4;
	var.lVal = value;
	return codec_api->SetValue(&property, &var);
}

ComPtr<IMFSample> create_sample(const uint8_t *data, uint32_t size,
                                LONGLONG timestamp_100ns) {
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
	sample->SetSampleDuration(0);

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
	bool configure_codec_properties();
	void drain_encoder();

	ComPtr<IMFTransform> transform_;
	ComPtr<ICodecAPI> codec_api_;
	ComPtr<IMFMediaType> output_type_;

	int width_{0};
	int height_{0};
	int bitrate_bps_{0};
	int fps_{0};

	std::vector<uint8_t> sps_pps_;
	bool async_mft_{false};

	static constexpr DWORD kStream_id = 0;
};

mf_video_encoder::~mf_video_encoder() {
	drain_encoder();
}

bool mf_video_encoder::init(int width, int height, int bitrate_bps, int fps) {
	width_ = width;
	height_ = height;
	bitrate_bps_ = bitrate_bps;
	fps_ = fps;
	sps_pps_.clear();

	HRESULT hr = MFStartup(MF_VERSION);
	if(FAILED(hr)) {
		return false;
	}

	if(!create_transform()) {
		return false;
	}

	if(!configure_input_type()) {
		return false;
	}

	if(!configure_output_type()) {
		return false;
	}

	if(!configure_codec_properties()) {
		return false;
	}

	hr = transform_->QueryInterface(IID_PPV_ARGS(&codec_api_));
	if(SUCCEEDED(hr)) {
		IMFAttributes *attributes = nullptr;
		hr = transform_->GetAttributes(&attributes);
		if(SUCCEEDED(hr)) {
			UINT32 is_async = 0;
			hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
			if(SUCCEEDED(hr) && is_async) {
				hr = transform_->ProcessMessage(
				    MFT_MESSAGE_NOTIFY_START_OF_STREAM, nullptr);
				if(SUCCEEDED(hr)) {
					async_mft_ = true;
				}
			}
			attributes->Release();
		}
	}

	hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, nullptr);
	// Non-fatal on some implementations.

	return true;
}

bool mf_video_encoder::create_transform() {
	MFT_REGISTER_TYPE_INFO output_type_info{};
	output_type_info.guidMajorType = MFMediaType_Video;
	output_type_info.guidSubtype = MFVideoFormat_H264;

	IMFActivate **activates = nullptr;
	UINT32 activate_count = 0;

	HRESULT hr = MFTEnumEx(
	    MFT_CATEGORY_VIDEO_ENCODER,
	    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
	    nullptr, &output_type_info,
	    &activates, &activate_count);

	if(FAILED(hr) || activate_count == 0) {
		hr = MFTEnumEx(
		    MFT_CATEGORY_VIDEO_ENCODER,
		    MFT_ENUM_FLAG_SORTANDFILTER,
		    nullptr, &output_type_info,
		    &activates, &activate_count);
	}

	if(FAILED(hr) || activate_count == 0) {
		return false;
	}

	hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));

	for(UINT32 i = 0; i < activate_count; i++) {
		activates[i]->Release();
	}
	CoTaskMemFree(activates);

	if(FAILED(hr)) {
		return false;
	}

	return true;
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

	hr = transform_->SetOutputType(kStream_id, output_type.Get(), 0);
	if(FAILED(hr)) return false;

	hr = transform_->GetOutputType(kStream_id, &output_type_);
	if(FAILED(hr)) return false;

	extract_sps_pps_from_sequence_header(output_type_.Get(), sps_pps_);

	return true;
}

bool mf_video_encoder::configure_codec_properties() {
	ComPtr<ICodecAPI> codec_api;
	HRESULT hr = transform_->QueryInterface(IID_PPV_ARGS(&codec_api));
	if(FAILED(hr)) {
		return true;
	}

	set_codec_property(codec_api.Get(), CODECAPI_AVEncCommonLowLatency, TRUE);

	hr = set_codec_property(codec_api.Get(),
	                        CODECAPI_AVEncCommonMeanBitRate,
	                        static_cast<LONG>(bitrate_bps_));

	return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
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
	    nv12.data(), static_cast<uint32_t>(nv12_frame_size), timestamp_100ns);
	if(!input_sample) {
		return false;
	}

	HRESULT hr = transform_->ProcessInput(kStream_id, input_sample.Get(), 0);
	if(FAILED(hr)) {
		return false;
	}

	MFT_OUTPUT_DATA_BUFFER output_buffer{};
	output_buffer.dwStreamID = kStream_id;
	DWORD status = 0;

	while(true) {
		output_buffer.pSample = nullptr;

		hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);

		if(hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			break;
		}

		if(hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			ComPtr<IMFMediaType> new_output_type;
			hr = transform_->GetOutputType(kStream_id, &new_output_type);
			if(SUCCEEDED(hr)) {
				output_type_ = new_output_type;
				sps_pps_.clear();
				extract_sps_pps_from_sequence_header(output_type_.Get(),
				                                     sps_pps_);
			}
			if(output_buffer.pSample) {
				output_buffer.pSample->Release();
			}
			continue;
		}

		if(FAILED(hr)) {
			if(output_buffer.pSample) {
				output_buffer.pSample->Release();
			}
			break;
		}

		if(!output_buffer.pSample) {
			break;
		}

		ComPtr<IMFMediaBuffer> media_buffer;
		hr = output_buffer.pSample->ConvertToContiguousBuffer(&media_buffer);
		if(SUCCEEDED(hr)) {
			BYTE *data_ptr = nullptr;
			DWORD data_length = 0;
			hr = media_buffer->Lock(&data_ptr, nullptr, &data_length);
			if(SUCCEEDED(hr)) {
				encoded_frame frame;
				frame.timestamp_us = timestamp_us;
				frame.is_keyframe = is_keyframe(output_buffer.pSample.Get());

				if(frame.is_keyframe && !sps_pps_.empty()) {
					frame.data.insert(frame.data.end(),
					                  sps_pps_.begin(), sps_pps_.end());
				}

				uint32_t nal_length_size = 4;
				if(output_type_) {
					UINT32 header_size = 0;
					HRESULT blob_hr = output_type_->GetBlob(
					    MF_MT_MPEG_SEQUENCE_HEADER, nullptr, 0, &header_size);
					if(SUCCEEDED(blob_hr) && header_size >= 5) {
						std::vector<uint8_t> header_buf(header_size);
						output_type_->GetBlob(
						    MF_MT_MPEG_SEQUENCE_HEADER,
						    header_buf.data(), header_size, &header_size);
						nal_length_size = (header_buf[4] & 0x03) + 1;
					}
				}

				avcc_to_annex_b(data_ptr, data_length, nal_length_size,
				                frame.data);

				media_buffer->Unlock();

				if(encoded_callback_ && !frame.data.empty()) {
					encoded_callback_(frame);
				}
			}
		}

		output_buffer.pSample->Release();
	}

	return true;
}

void mf_video_encoder::drain_encoder() {
	if(!transform_) {
		return;
	}

	transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, nullptr);

	MFT_OUTPUT_DATA_BUFFER output_buffer{};
	output_buffer.dwStreamID = kStream_id;
	DWORD status = 0;

	for(int i = 0; i < 10; i++) {
		output_buffer.pSample = nullptr;

		HRESULT hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);
		if(hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
			break;
		}
		if(output_buffer.pSample) {
			output_buffer.pSample->Release();
		}
	}

	MFShutdown();
}

std::unique_ptr<video_encoder> create_video_encoder() {
	return std::make_unique<mf_video_encoder>();
}

} // namespace webrtc
} // namespace bbb
