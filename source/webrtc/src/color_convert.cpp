#include "bbb/color_convert.hpp"

#include <algorithm>
#include <cstdint>

namespace bbb {
namespace webrtc {

static inline uint8_t clip_uint8(int val) {
	return static_cast<uint8_t>(std::max(0, std::min(255, val)));
}

void rgba_to_nv12(const uint8_t *rgba, int width, int height, int stride,
                  std::vector<uint8_t> &nv12_out) {
	nv12_out.resize(width * height + width * (height / 2));

	for(int y = 0; y < height; ++y) {
		for(int x = 0; x < width; ++x) {
			int r = rgba[y * stride + x * 4 + 0];
			int g = rgba[y * stride + x * 4 + 1];
			int b = rgba[y * stride + x * 4 + 2];

			int y_val = (77 * r + 150 * g + 29 * b + 128) >> 8;
			nv12_out[y * width + x] = clip_uint8(y_val);
		}
	}

	uint8_t *uv_plane = nv12_out.data() + width * height;
	for(int y = 0; y < height / 2; ++y) {
		for(int x = 0; x < width; x += 2) {
			int r00 = rgba[(y * 2) * stride + (x * 2) * 4 + 0];
			int g00 = rgba[(y * 2) * stride + (x * 2) * 4 + 1];
			int b00 = rgba[(y * 2) * stride + (x * 2) * 4 + 2];

			int r01 = rgba[(y * 2) * stride + (x * 2 + 1) * 4 + 0];
			int g01 = rgba[(y * 2) * stride + (x * 2 + 1) * 4 + 1];
			int b01 = rgba[(y * 2) * stride + (x * 2 + 1) * 4 + 2];

			int r10 = rgba[(y * 2 + 1) * stride + (x * 2) * 4 + 0];
			int g10 = rgba[(y * 2 + 1) * stride + (x * 2) * 4 + 1];
			int b10 = rgba[(y * 2 + 1) * stride + (x * 2) * 4 + 2];

			int r11 = rgba[(y * 2 + 1) * stride + (x * 2 + 1) * 4 + 0];
			int g11 = rgba[(y * 2 + 1) * stride + (x * 2 + 1) * 4 + 1];
			int b11 = rgba[(y * 2 + 1) * stride + (x * 2 + 1) * 4 + 2];

			int avg_r = (r00 + r01 + r10 + r11 + 2) / 4;
			int avg_g = (g00 + g01 + g10 + g11 + 2) / 4;
			int avg_b = (b00 + b01 + b10 + b11 + 2) / 4;

			int u = ((-43 * avg_r - 85 * avg_g + 128 * avg_b + 128) >> 8) + 128;
			int v = ((128 * avg_r - 107 * avg_g - 21 * avg_b + 128) >> 8) + 128;

			uv_plane[y * width + x + 0] = clip_uint8(u);
			uv_plane[y * width + x + 1] = clip_uint8(v);
		}
	}
}

void nv12_to_rgba(const uint8_t *nv12, int width, int height,
                  uint8_t *rgba_out, int stride) {
	const uint8_t *y_plane = nv12;
	const uint8_t *uv_plane = nv12 + width * height;

	for(int y = 0; y < height; ++y) {
		for(int x = 0; x < width; ++x) {
			int y_val = y_plane[y * width + x];
			int uv_idx = (y / 2) * width + (x & ~1);
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			int r = y_val + ((v * 1436) >> 10);
			int g = y_val - ((u * 352 + v * 731) >> 10);
			int b = y_val + ((u * 1815) >> 10);

			rgba_out[y * stride + x * 4 + 0] = clip_uint8(r);
			rgba_out[y * stride + x * 4 + 1] = clip_uint8(g);
			rgba_out[y * stride + x * 4 + 2] = clip_uint8(b);
			rgba_out[y * stride + x * 4 + 3] = 255;
		}
	}
}

} // namespace webrtc
} // namespace bbb
