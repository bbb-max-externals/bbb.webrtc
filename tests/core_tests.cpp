#include "bbb/audio_ring_buffer.hpp"
#include "bbb/color_convert.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failure_count = 0;

void expect(bool condition, const std::string &message) {
	if(!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failure_count;
	}
}

template <typename value_type>
void expect_equal(const value_type &actual, const value_type &expected, const std::string &message) {
	if(!(actual == expected)) {
		std::cerr << "FAIL: " << message << '\n';
		++failure_count;
	}
}

void test_audio_ring_buffer_capacity_wrap_and_reset() {
	bbb::webrtc::audio_ring_buffer buffer{4};
	expect_equal(buffer.available_read(), std::size_t{0}, "new buffer has no readable samples");
	expect_equal(buffer.available_write(), std::size_t{4}, "new buffer exposes requested writable capacity");

	const float first_samples[]{1.0f, 2.0f, 3.0f};
	expect_equal(buffer.write(first_samples, 3), std::size_t{3}, "initial write count");
	expect_equal(buffer.available_read(), std::size_t{3}, "readable after initial write");
	expect_equal(buffer.available_write(), std::size_t{1}, "writable after initial write");

	float first_read[2]{};
	expect_equal(buffer.read(first_read, 2), std::size_t{2}, "partial read count");
	expect_equal(first_read[0], 1.0f, "first read sample 0");
	expect_equal(first_read[1], 2.0f, "first read sample 1");

	const float wrapped_samples[]{4.0f, 5.0f, 6.0f};
	expect_equal(buffer.write(wrapped_samples, 3), std::size_t{3}, "wrap write fills remaining capacity");
	expect_equal(buffer.available_write(), std::size_t{0}, "full buffer has no writable space");

	float wrapped_read[4]{};
	expect_equal(buffer.read(wrapped_read, 4), std::size_t{4}, "wrap read count");
	expect_equal(wrapped_read[0], 3.0f, "wrapped read keeps unread sample");
	expect_equal(wrapped_read[1], 4.0f, "wrapped read sample 1");
	expect_equal(wrapped_read[2], 5.0f, "wrapped read sample 2");
	expect_equal(wrapped_read[3], 6.0f, "wrapped read sample 3");
	expect_equal(buffer.available_read(), std::size_t{0}, "buffer empty after read");

	const float too_many_samples[]{7.0f, 8.0f, 9.0f, 10.0f, 11.0f};
	expect_equal(buffer.write(too_many_samples, 5), std::size_t{4}, "write truncates to capacity");
	buffer.reset();
	expect_equal(buffer.available_read(), std::size_t{0}, "reset clears readable samples");
	expect_equal(buffer.available_write(), std::size_t{4}, "reset restores writable capacity");
}

void test_rgba_to_nv12_solid_red() {
	constexpr int width = 2;
	constexpr int height = 2;
	constexpr int stride = width * 4;
	const std::vector<std::uint8_t> rgba{
		255, 0, 0, 255, 255, 0, 0, 255,
		255, 0, 0, 255, 255, 0, 0, 255,
	};

	std::vector<std::uint8_t> nv12;
	bbb::webrtc::rgba_to_nv12(rgba.data(), width, height, stride, nv12);
	expect_equal(nv12.size(), std::size_t{6}, "2x2 NV12 buffer size");
	expect_equal(nv12[0], std::uint8_t{77}, "red Y 0");
	expect_equal(nv12[1], std::uint8_t{77}, "red Y 1");
	expect_equal(nv12[2], std::uint8_t{77}, "red Y 2");
	expect_equal(nv12[3], std::uint8_t{77}, "red Y 3");
	expect_equal(nv12[4], std::uint8_t{85}, "red U");
	expect_equal(nv12[5], std::uint8_t{255}, "red V");
}

void test_rgba_to_nv12_chroma_pairs_use_source_pixel_coordinates() {
	constexpr int width = 4;
	constexpr int height = 2;
	constexpr int stride = width * 4;
	const std::vector<std::uint8_t> rgba{
		255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
		255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
	};

	std::vector<std::uint8_t> nv12;
	bbb::webrtc::rgba_to_nv12(rgba.data(), width, height, stride, nv12);
	expect_equal(nv12.size(), std::size_t{12}, "4x2 NV12 buffer size");
	expect_equal(nv12[0], std::uint8_t{77}, "mixed Y red 0");
	expect_equal(nv12[1], std::uint8_t{77}, "mixed Y red 1");
	expect_equal(nv12[2], std::uint8_t{149}, "mixed Y green 0");
	expect_equal(nv12[3], std::uint8_t{149}, "mixed Y green 1");
	expect_equal(nv12[8], std::uint8_t{85}, "mixed red U");
	expect_equal(nv12[9], std::uint8_t{255}, "mixed red V");
	expect_equal(nv12[10], std::uint8_t{43}, "mixed green U");
	expect_equal(nv12[11], std::uint8_t{21}, "mixed green V");
}

void test_nv12_to_rgba_neutral_gray() {
	constexpr int width = 2;
	constexpr int height = 2;
	constexpr int stride = width * 4;
	const std::vector<std::uint8_t> nv12{
		128, 128,
		128, 128,
		128, 128,
	};
	std::vector<std::uint8_t> rgba(width * height * 4, 0);

	bbb::webrtc::nv12_to_rgba(nv12.data(), width, height, rgba.data(), stride);
	for(std::size_t index = 0; index < rgba.size(); index += 4) {
		expect_equal(rgba[index + 0], std::uint8_t{128}, "gray R");
		expect_equal(rgba[index + 1], std::uint8_t{128}, "gray G");
		expect_equal(rgba[index + 2], std::uint8_t{128}, "gray B");
		expect_equal(rgba[index + 3], std::uint8_t{255}, "gray A");
	}
}

} // namespace

int main() {
	test_audio_ring_buffer_capacity_wrap_and_reset();
	test_rgba_to_nv12_solid_red();
	test_rgba_to_nv12_chroma_pairs_use_source_pixel_coordinates();
	test_nv12_to_rgba_neutral_gray();

	if(failure_count != 0) {
		std::cerr << failure_count << " test assertion(s) failed\n";
		return 1;
	}

	std::cout << "bbb.webrtc core tests passed\n";
	return 0;
}
