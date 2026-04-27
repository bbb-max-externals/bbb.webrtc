#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace bbb {
namespace webrtc {

// single-producer single-consumer lock-free ring buffer for audio samples
class audio_ring_buffer {
public:
	explicit audio_ring_buffer(std::size_t capacity);

	// write samples (called from network/decoder thread)
	// returns number of samples actually written
	std::size_t write(const float *data, std::size_t count);

	// read samples (called from audio thread)
	// returns number of samples actually read
	std::size_t read(float *data, std::size_t count);

	// returns available samples for reading
	std::size_t available_read() const;

	// returns available space for writing
	std::size_t available_write() const;

	void reset();

private:
	std::vector<float> buffer_;
	std::size_t capacity_;
	std::atomic<std::size_t> write_pos_{0};
	std::atomic<std::size_t> read_pos_{0};
};

} // namespace webrtc
} // namespace bbb
