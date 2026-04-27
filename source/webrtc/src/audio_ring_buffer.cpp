#include "bbb/audio_ring_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace bbb {
namespace webrtc {

audio_ring_buffer::audio_ring_buffer(std::size_t capacity)
	: buffer_(capacity + 1, 0.0f)
	, capacity_(capacity + 1) {}

std::size_t audio_ring_buffer::write(const float *data, std::size_t count) {
	std::size_t available = available_write();
	std::size_t to_write = (count < available) ? count : available;

	std::size_t write_pos = write_pos_.load(std::memory_order_relaxed);

	for(std::size_t i = 0; i < to_write; ++i) {
		buffer_[write_pos] = data[i];
		write_pos = (write_pos + 1) % capacity_;
	}

	write_pos_.store(write_pos, std::memory_order_release);
	return to_write;
}

std::size_t audio_ring_buffer::read(float *data, std::size_t count) {
	std::size_t available = available_read();
	std::size_t to_read = (count < available) ? count : available;

	std::size_t read_pos = read_pos_.load(std::memory_order_relaxed);

	for(std::size_t i = 0; i < to_read; ++i) {
		data[i] = buffer_[read_pos];
		read_pos = (read_pos + 1) % capacity_;
	}

	read_pos_.store(read_pos, std::memory_order_release);
	return to_read;
}

std::size_t audio_ring_buffer::available_read() const {
	std::size_t write_pos = write_pos_.load(std::memory_order_acquire);
	std::size_t read_pos = read_pos_.load(std::memory_order_acquire);
	if(write_pos >= read_pos) {
		return write_pos - read_pos;
	}
	return capacity_ - read_pos + write_pos;
}

std::size_t audio_ring_buffer::available_write() const {
	return capacity_ - 1 - available_read();
}

void audio_ring_buffer::reset() {
	write_pos_.store(0, std::memory_order_relaxed);
	read_pos_.store(0, std::memory_order_relaxed);
}

} // namespace webrtc
} // namespace bbb
