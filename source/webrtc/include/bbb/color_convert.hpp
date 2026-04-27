#pragma once

#include <cstdint>
#include <vector>

namespace bbb {
namespace webrtc {

// RGBA (4 bytes/pixel, stride bytes per row) -> NV12 (Y plane + interleaved UV plane)
void rgba_to_nv12(const uint8_t *rgba, int width, int height, int stride,
                  std::vector<uint8_t> &nv12_out);

// NV12 (Y plane width*height, UV plane width*(height/2)) -> RGBA (4 bytes/pixel, stride bytes per row)
void nv12_to_rgba(const uint8_t *nv12, int width, int height,
                  uint8_t *rgba_out, int stride);

} // namespace webrtc
} // namespace bbb
