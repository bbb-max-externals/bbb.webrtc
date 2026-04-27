#include "bbb/video_decoder.hpp"
#include "bbb/color_convert.hpp"

// NIL macro collides with Windows headers used by Media Foundation.
#pragma push_macro("NIL")
#undef NIL

#include <wrl/client.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>

#include <mferror.h>

#pragma pop_macro("NIL")

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bbb {
namespace webrtc {

namespace {

static const uint8_t kAnnexBStartCode3[3] = {0x00, 0x00, 0x01};
static const uint8_t kAnnexBStartCode4[4] = {0x00, 0x00, 0x00, 0x01};

bool feed_annex_b_nals(IMFTransform *transform, DWORD stream_id,
                       const uint8_t *data, size_t size,
                       LONGLONG timestamp_100ns) {
	size_t pos = 0;
	bool fed_any = false;

	while(pos < size) {
		const uint8_t *start = data + pos;
		const uint8_t *end = data + size;

		const uint8_t *next_start = nullptr;
		for(size_t i = pos; i + 3 < size; i++) {
			if(data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
				if(i > 0 && data[i - 1] == 0x00) {
					continue;
				}
				next_start = data + i;
				break;
			}
		}

		size_t nal_size = 0;
		if(next_start) {
			nal_size = static_cast<size_t>(next_start - start);
		} else {
			nal_size = static_cast<size_t>(end - start);
		}

		const uint8_t *nal_data = start;
		size_t nal_data_size = nal_size;
		if(nal_size >= 4 && std::memcmp(nal_data, kAnnexBStartCode4, 4) == 0) {
			nal_data += 4;
			nal_data_size -= 4;
		} else if(nal_size >= 3 && std::memcmp(nal_data, kAnnexBStartCode3, 3) == 0) {
			nal_data += 3;
			nal_data_size -= 3;
		}

		if(nal_data_size > 0) {
			ComPtr<IMFSample> sample;
			HRESULT hr = MFCreateSample(&sample);
			if(SUCCEEDED(hr)) {
				ComPtr<IMFMediaBuffer> buffer;
				hr = MFCreateMemoryBuffer(
				    static_cast<DWORD>(nal_data_size), &buffer);
				if(SUCCEEDED(hr)) {
					BYTE *buf_ptr = nullptr;
					hr = buffer->Lock(&buf_ptr, nullptr, nullptr);
					if(SUCCEEDED(hr)) {
						std::memcpy(buf_ptr, nal_data, nal_data_size);
						buffer->Unlock();
						buffer->SetCurrentLength(
						    static_cast<DWORD>(nal_data_size));
						sample->AddBuffer(buffer.Get());
						sample->SetSampleTime(timestamp_100ns);

						hr = transform->ProcessInput(stream_id, sample.Get(), 0);
						if(SUCCEEDED(hr)) {
							fed_any = true;
						}
					}
				}
			}
		}

		if(next_start) {
			pos = static_cast<size_t>(next_start - data);
		} else {
			break;
		}
	}

	return fed_any;
}

} // anonymous namespace

class mf_video_decoder : public video_decoder {
public:
	mf_video_decoder() = default;
	~mf_video_decoder() override;

	bool init() override;
	bool decode(const uint8_t *nal_data, size_t nal_size,
	            uint64_t timestamp_us) override;

private:
	bool create_transform();
	bool configure_input_type();
	bool configure_output_type();
	bool handle_stream_change();

	void drain_output(LONGLONG timestamp_100ns);

	ComPtr<IMFTransform> transform_;
	ComPtr<IMFMediaType> output_type_;

	int decoded_width_{0};
	int decoded_height_{0};
	bool stream_started_{false};

	static constexpr DWORD kStream_id = 0;
};

mf_video_decoder::~mf_video_decoder() {
	if(transform_) {
		transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
	}
	MFShutdown();
}

bool mf_video_decoder::init() {
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

	hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

	return true;
}

bool mf_video_decoder::create_transform() {
	MFT_REGISTER_TYPE_INFO input_type_info{};
	input_type_info.guidMajorType = MFMediaType_Video;
	input_type_info.guidSubtype = MFVideoFormat_H264;

	IMFActivate **activates = nullptr;
	UINT32 activate_count = 0;

	HRESULT hr = MFTEnumEx(
	    MFT_CATEGORY_VIDEO_DECODER,
	    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
	    &input_type_info, nullptr,
	    &activates, &activate_count);

	if(FAILED(hr) || activate_count == 0) {
		hr = MFTEnumEx(
		    MFT_CATEGORY_VIDEO_DECODER,
		    MFT_ENUM_FLAG_SORTANDFILTER,
		    &input_type_info, nullptr,
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

bool mf_video_decoder::configure_input_type() {
	ComPtr<IMFMediaType> input_type;
	HRESULT hr = MFCreateMediaType(&input_type);
	if(FAILED(hr)) {
		return false;
	}

	hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if(FAILED(hr)) return false;

	hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	if(FAILED(hr)) return false;

	hr = transform_->SetInputType(kStream_id, input_type.Get(), 0);
	return SUCCEEDED(hr);
}

bool mf_video_decoder::configure_output_type() {
	ComPtr<IMFMediaType> output_type;
	HRESULT hr = MFCreateMediaType(&output_type);
	if(FAILED(hr)) {
		return false;
	}

	hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if(FAILED(hr)) return false;

	hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	if(FAILED(hr)) return false;

	hr = transform_->SetOutputType(kStream_id, output_type.Get(), 0);
	if(FAILED(hr)) return false;

	hr = transform_->GetOutputAvailableType(kStream_id, 0, &output_type_);
	if(FAILED(hr)) return false;

	UINT32 width = 0, height = 0;
	hr = MFGetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE,
	                        &width, &height);
	if(SUCCEEDED(hr)) {
		decoded_width_ = static_cast<int>(width);
		decoded_height_ = static_cast<int>(height);
	}

	return true;
}

bool mf_video_decoder::handle_stream_change() {
	if(!configure_output_type()) {
		return false;
	}

	HRESULT hr = transform_->ProcessMessage(
	    MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, nullptr);

	return SUCCEEDED(hr) || hr == E_NOTIMPL;
}

void mf_video_decoder::drain_output(LONGLONG timestamp_100ns) {
	if(!transform_ || decoded_width_ == 0 || decoded_height_ == 0) {
		return;
	}

	MFT_OUTPUT_DATA_BUFFER output_buffer{};
	output_buffer.dwStreamID = kStream_id;
	DWORD status = 0;

	while(true) {
		output_buffer.pSample = nullptr;

		HRESULT hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);

		if(hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			break;
		}

		if(hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			handle_stream_change();
			if(output_buffer.pSample) {
				output_buffer.pSample->Release();
			}
			continue;
		}

		if(FAILED(hr) || !output_buffer.pSample) {
			if(output_buffer.pSample) {
				output_buffer.pSample->Release();
			}
			break;
		}

		ComPtr<IMFMediaBuffer> media_buffer;
		hr = output_buffer.pSample->ConvertToContiguousBuffer(&media_buffer);
		if(SUCCEEDED(hr)) {
			BYTE *data_ptr = nullptr;
			DWORD max_length = 0;
			DWORD current_length = 0;
			hr = media_buffer->Lock(&data_ptr, &max_length, &current_length);
			if(SUCCEEDED(hr)) {
				int width = decoded_width_;
				int height = decoded_height_;

				ComPtr<IMF2DBuffer> buffer_2d;
				if(SUCCEEDED(media_buffer.As(&buffer_2d))) {
					BYTE *scanline0 = nullptr;
					LONG pitch = 0;
					hr = buffer_2d->Lock2D(&scanline0, &pitch);
					if(SUCCEEDED(hr)) {
						// HW decoders return pitch-padded 2D buffers;
						// nv12_to_rgba expects contiguous row-major layout.
						std::vector<uint8_t> nv12(width * height + width * (height / 2));
						for(int row = 0; row < height; row++) {
							std::memcpy(nv12.data() + row * width,
							           scanline0 + row * pitch, width);
						}
						BYTE *uv_start = scanline0 + static_cast<LONGLONG>(height) * pitch;
						for(int row = 0; row < height / 2; row++) {
							std::memcpy(nv12.data() + width * height + row * width,
							           uv_start + row * pitch, width);
						}
						buffer_2d->Unlock2D();

						decoded_frame frame;
						frame.width = width;
						frame.height = height;
						frame.timestamp_us = static_cast<uint64_t>(timestamp_100ns / 10);
						frame.data.resize(width * height * 4);

						nv12_to_rgba(nv12.data(), width, height,
						             frame.data.data(), width * 4);

						if(decoded_callback_) {
							decoded_callback_(frame);
						}
					}
				} else {
					decoded_frame frame;
					frame.width = width;
					frame.height = height;
					frame.timestamp_us = static_cast<uint64_t>(timestamp_100ns / 10);
					frame.data.resize(width * height * 4);

					nv12_to_rgba(data_ptr, width, height,
					             frame.data.data(), width * 4);

					media_buffer->Unlock();

					if(decoded_callback_) {
						decoded_callback_(frame);
					}
				}
			}
		}

		output_buffer.pSample->Release();
	}
}

bool mf_video_decoder::decode(const uint8_t *nal_data, size_t nal_size,
                               uint64_t timestamp_us) {
	if(!transform_) {
		return false;
	}

	LONGLONG timestamp_100ns = static_cast<LONGLONG>(timestamp_us) * 10;

	if(!stream_started_) {
		HRESULT hr = transform_->ProcessMessage(
		    MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		if(FAILED(hr)) {
			return false;
		}

		configure_output_type();
		stream_started_ = true;
	}

	bool fed = feed_annex_b_nals(transform_.Get(), kStream_id,
	                             nal_data, nal_size, timestamp_100ns);
	if(!fed) {
		return false;
	}

	drain_output(timestamp_100ns);
	return true;
}

std::unique_ptr<video_decoder> create_video_decoder() {
	return std::make_unique<mf_video_decoder>();
}

} // namespace webrtc
} // namespace bbb
