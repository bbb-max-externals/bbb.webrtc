#include "c74_min.h"
#include "bbb/video_session.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace c74::min;

static std::string to_string(const symbol &s) {
	return std::string(s.c_str());
}

class bbb_webrtc_send_video : public object<bbb_webrtc_send_video>, public matrix_operator<> {
public:
	MIN_DESCRIPTION{"Send video via WebRTC"};
	MIN_TAGS{"webrtc, video, network, jitter"};
	MIN_AUTHOR{"ISHII 2bit"};

	outlet<> message_out{this, "(anything) SDP, ICE candidates, status"};

	attribute<symbol> stun_server{this, "stun_server", "stun:stun.l.google.com:19302",
		description{"STUN server URL"}
	};

	attribute<symbol> turn_server{this, "turn_server", "",
		description{"TURN server URL (empty = no TURN)"}
	};

	attribute<symbol> turn_username{this, "turn_username", "",
		description{"TURN username"}
	};

	attribute<symbol> turn_password{this, "turn_password", "",
		description{"TURN password"}
	};

	attribute<int> bitrate{this, "bitrate", 2000000,
		description{"H.264 bitrate in bps"},
		range{100000, 10000000}
	};

	attribute<int> width{this, "width", 640,
		description{"Video width"},
		range{16, 3840}
	};

	attribute<int> height{this, "height", 480,
		description{"Video height"},
		range{16, 2160}
	};

	attribute<int> fps{this, "fps", 30,
		description{"Video frame rate"},
		range{1, 60}
	};

	bbb_webrtc_send_video(const atoms &args = {}) {
		m_init_timer.delay(0);
	}

	~bbb_webrtc_send_video() {
		close();
	}

	message<> offer_msg{this, "offer", "Create SDP offer and start sending",
		MIN_FUNCTION {
			create_offer();
			return {};
		}
	};

	message<> answer_msg{this, "answer", "Set remote SDP answer: answer <sdp>",
		MIN_FUNCTION {
			if(args.size() < 1) return {};
			set_remote_answer(to_string(symbol(args[0])));
			return {};
		}
	};

	message<> candidate_msg{this, "candidate", "Add remote ICE candidate: candidate <candidate> <mid>",
		MIN_FUNCTION {
			if(args.size() < 2) return {};
			add_remote_ice(to_string(symbol(args[0])), to_string(symbol(args[1])));
			return {};
		}
	};

	message<> close_msg{this, "close", "Close the connection",
		MIN_FUNCTION {
			close();
			return {};
		}
	};

	message<> dump_msg{this, "dump", "Print status",
		MIN_FUNCTION {
			cout << "bbb.webrtc.send.video status:" << endl;
			cout << "  state: " << state_string() << endl;
			cout << "  stun: " << to_string(stun_server) << endl;
			cout << "  resolution: " << static_cast<int>(width) << "x" << static_cast<int>(height) << endl;
			cout << "  bitrate: " << static_cast<int>(bitrate) << endl;
			cout << "  fps: " << static_cast<int>(fps) << endl;
			return {};
		}
	};

	template <class matrix_type, size_t plane_count>
	cell<matrix_type, plane_count> calc_cell(cell<matrix_type, plane_count> input,
	                                         const matrix_info &info, matrix_coord &position) {
		long x = position.x();
		long y = position.y();

		if(x == 0 && y == 0) {
			capture_frame(info);
		}

		return input;
	}

private:
	std::unique_ptr<bbb::webrtc::video_session> m_session;
	std::vector<uint8_t> m_frame_buffer;
	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_frame_captured{false};

	timer<timer_options::defer_delivery> m_init_timer{this,
		MIN_FUNCTION {
			return {};
		}
	};

	queue<> m_message_queue{this, MIN_FUNCTION {
		deliver_pending_messages();
		return {};
	}};

	struct pending_message {
		std::string selector;
		std::vector<std::string> args;
	};
	std::mutex m_pending_mutex;
	std::vector<pending_message> m_pending_messages;

	bbb::webrtc::video_session_config make_config() {
		bbb::webrtc::video_session_config cfg;
		auto stun = to_string(stun_server);
		if(!stun.empty()) {
			cfg.ice_servers.push_back({stun, "", ""});
		}
		auto turn = to_string(turn_server);
		if(!turn.empty()) {
			cfg.ice_servers.push_back({turn, to_string(turn_username), to_string(turn_password)});
		}
		cfg.width = static_cast<int>(width);
		cfg.height = static_cast<int>(height);
		cfg.bitrate_bps = static_cast<int>(bitrate);
		cfg.fps = static_cast<int>(fps);
		return cfg;
	}

	void capture_frame(const matrix_info &info) {
		long w = static_cast<long>(info.width());
		long h = static_cast<long>(info.height());

		if(w != static_cast<long>(width) || h != static_cast<long>(height)) {
			return;
		}

		long row_bytes = w * info.m_in_info->planecount;
		m_frame_buffer.resize(static_cast<std::size_t>(row_bytes * h));
		auto src = info.m_bip;
		auto dst = m_frame_buffer.data();
		for(long y = 0; y < h; ++y) {
			std::memcpy(dst + static_cast<std::size_t>(y) * row_bytes,
			            src + static_cast<std::size_t>(y) * info.m_in_info->dimstride[1],
			            static_cast<std::size_t>(row_bytes));
		}
		m_frame_captured.store(true);
	}

	void create_offer() {
		close();
		auto cfg = make_config();
		m_session = std::make_unique<bbb::webrtc::video_session>(cfg);

		m_session->on_local_description([this](const std::string &type, const std::string &sdp) {
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			m_pending_messages.push_back({type, {sdp}});
			m_message_queue.set();
		});

		m_session->on_local_ice_candidate([this](const std::string &candidate, const std::string &mid) {
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			m_pending_messages.push_back({"candidate", {candidate, mid}});
			m_message_queue.set();
		});

		m_session->on_state_change([this](bbb::webrtc::video_session::state state) {
			m_connected.store(state == bbb::webrtc::video_session::state::connected);
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			m_pending_messages.push_back({"state", {state_to_string(state)}});
			m_message_queue.set();
		});

		m_session->create_offer();
	}

	void send_captured_frame() {
		if(!m_session || !m_connected.load(std::memory_order_acquire)) return;
		if(!m_frame_captured.load(std::memory_order_acquire)) return;

		int w = static_cast<int>(width);
		int h = static_cast<int>(height);
		m_session->send_video_frame(m_frame_buffer.data(), w * 4, w, h);
		m_frame_captured.store(false, std::memory_order_release);
	}

	void set_remote_answer(const std::string &sdp) {
		if(m_session) {
			m_session->set_remote_description("answer", sdp);
		}
	}

	void add_remote_ice(const std::string &candidate, const std::string &mid) {
		if(m_session) {
			m_session->add_ice_candidate(candidate, mid);
		}
	}

	void close() {
		m_connected.store(false, std::memory_order_release);
		if(m_session) {
			m_session->close();
			m_session.reset();
		}
		m_frame_buffer.clear();
		m_frame_captured.store(false);
	}

	void deliver_pending_messages() {
		std::vector<pending_message> msgs;
		{
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			msgs.swap(m_pending_messages);
		}
		for(const auto &msg : msgs) {
			atoms a;
			for(const auto &arg : msg.args) {
				a.push_back(atom(symbol(arg)));
			}
			a.insert(a.begin(), atom(symbol(msg.selector)));
			message_out.send(a);
		}
	}

	static std::string state_to_string(bbb::webrtc::video_session::state state) {
		switch(state) {
			case bbb::webrtc::video_session::state::disconnected: return "disconnected";
			case bbb::webrtc::video_session::state::connecting: return "connecting";
			case bbb::webrtc::video_session::state::connected: return "connected";
			case bbb::webrtc::video_session::state::failed: return "failed";
			case bbb::webrtc::video_session::state::closed: return "closed";
		}
		return "unknown";
	}

	std::string state_string() const {
		if(!m_session) return "not initialized";
		return state_to_string(m_session->get_state());
	}
};

MIN_EXTERNAL(bbb_webrtc_send_video);
