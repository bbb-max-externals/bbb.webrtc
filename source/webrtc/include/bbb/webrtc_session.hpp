#pragma once

#include <rtc/rtc.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bbb {
namespace webrtc {

struct ice_server {
	std::string url;
	std::string username;
	std::string password;
};

struct session_config {
	std::vector<ice_server> ice_servers;
	int opus_sample_rate{48000};
	int opus_channels{1};
	int opus_bitrate{64000};
	int opus_frame_size{960}; // 20ms at 48kHz
};

class session {
public:
	enum class state {
		disconnected,
		connecting,
		connected,
		failed,
		closed
	};

	using state_callback = std::function<void(state)>;
	using sdp_callback = std::function<void(const std::string &type, const std::string &sdp)>;
	using ice_callback = std::function<void(const std::string &candidate, const std::string &mid)>;

	explicit session(const session_config &config);
	~session();

	session(const session &) = delete;
	session &operator=(const session &) = delete;

	// signaling
	void create_offer();
	void set_remote_description(const std::string &type, const std::string &sdp);
	void add_ice_candidate(const std::string &candidate, const std::string &mid);

	// audio i/o (called from audio thread)
	void send_audio(const float *samples, int frame_count, int channels);

	// audio output (called from audio thread, reads from ring buffer)
	int receive_audio(float *samples, int frame_count, int channels);

	// callbacks (called from network thread, user must bridge to main thread)
	void on_state_change(state_callback callback);
	void on_local_description(sdp_callback callback);
	void on_local_ice_candidate(ice_callback callback);
	void on_audio_received(std::function<void(const float *data, int frame_count, int channels)> callback);

	state get_state() const;
	void close();

private:
	void setup_peer_connection();
	void add_audio_track();
	void setup_track_callbacks(rtc::shared_ptr<rtc::Track> track);

	session_config config_;
	state state_{state::disconnected};

	std::shared_ptr<rtc::PeerConnection> pc_;
	rtc::shared_ptr<rtc::Track> audio_track_;

	state_callback state_callback_;
	sdp_callback sdp_callback_;
	ice_callback ice_callback_;
	std::function<void(const float *data, int frame_count, int channels)> audio_received_callback_;

	struct opus_encoder;
	struct opus_decoder;
	std::unique_ptr<opus_encoder> encoder_;
	std::unique_ptr<opus_decoder> decoder_;

	// audio buffering for receive path
	std::vector<float> decode_buffer_;
	int decode_buffer_pos_{0};
	int decode_buffer_frames_{0};

	mutable std::mutex mutex_;
};

} // namespace webrtc
} // namespace bbb
