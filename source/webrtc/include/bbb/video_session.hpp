#pragma once

#include "bbb/webrtc_session.hpp"
#include "bbb/video_encoder.hpp"
#include "bbb/video_decoder.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bbb {
namespace webrtc {

struct video_session_config {
	std::vector<ice_server> ice_servers;
	int width{640};
	int height{480};
	int bitrate_bps{2000000};
	int fps{30};
};

class video_session {
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
	using video_frame_callback = std::function<void(const uint8_t *rgba, int width, int height)>;

	explicit video_session(const video_session_config &config);
	~video_session();

	video_session(const video_session &) = delete;
	video_session &operator=(const video_session &) = delete;

	void create_offer();
	void set_remote_description(const std::string &type, const std::string &sdp);
	void add_ice_candidate(const std::string &candidate, const std::string &mid);

	void send_video_frame(const uint8_t *rgba_data, int stride, int width, int height);
	void request_keyframe();

	void on_state_change(state_callback callback);
	void on_local_description(sdp_callback callback);
	void on_local_ice_candidate(ice_callback callback);
	void on_video_frame(video_frame_callback callback);

	state get_state() const;
	void close();

private:
	void setup_peer_connection();
	void add_video_track();
	void setup_video_track_callbacks(rtc::shared_ptr<rtc::Track> track);

	video_session_config config_;
	std::atomic<state> state_{state::disconnected};

	std::shared_ptr<rtc::PeerConnection> pc_;
	rtc::shared_ptr<rtc::Track> video_track_;
	std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config_;

	state_callback state_callback_;
	sdp_callback sdp_callback_;
	ice_callback ice_callback_;
	video_frame_callback video_frame_callback_;

	std::unique_ptr<video_encoder> encoder_;
	std::unique_ptr<video_decoder> decoder_;

	mutable std::mutex mutex_;
	std::atomic<bool> shutting_down_{false};
};

} // namespace webrtc
} // namespace bbb
