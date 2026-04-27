#include "c74_min.h"
#include "bbb/video_session.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace c74::min;

static std::string to_string(const symbol &s) {
	return std::string(s.c_str());
}

class bbb_webrtc_recv_video : public object<bbb_webrtc_recv_video>, public matrix_operator<> {
public:
	MIN_DESCRIPTION{"Receive video via WebRTC"};
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

	bbb_webrtc_recv_video(const atoms &args = {}) {
		m_init_timer.delay(0);
	}

	~bbb_webrtc_recv_video() {
		close();
	}

	message<> offer_msg{this, "offer", "Set remote SDP offer: offer <sdp>",
		MIN_FUNCTION {
			if(args.size() < 1) return {};
			set_remote_offer(to_string(symbol(args[0])));
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
			cout << "bbb.webrtc.recv.video status:" << endl;
			cout << "  state: " << state_string() << endl;
			cout << "  stun: " << to_string(stun_server) << endl;
			cout << "  frame: " << (m_has_frame.load() ? "yes" : "no") << endl;
			return {};
		}
	};

	template <class matrix_type, size_t plane_count>
	cell<matrix_type, plane_count> calc_cell(cell<matrix_type, plane_count> input,
	                                         const matrix_info &info, matrix_coord &position) {
		if constexpr (plane_count == 4) {
			if(!m_has_frame.load(std::memory_order_acquire)) {
				return {static_cast<matrix_type>(0), static_cast<matrix_type>(0),
				        static_cast<matrix_type>(0), static_cast<matrix_type>(255)};
			}

			long x = position.x();
			long y = position.y();

			std::lock_guard<std::mutex> lock(m_frame_mutex);

			if(x >= m_frame_width || y >= m_frame_height) {
				return {static_cast<matrix_type>(0), static_cast<matrix_type>(0),
				        static_cast<matrix_type>(0), static_cast<matrix_type>(255)};
			}

			std::size_t offset = static_cast<std::size_t>(y * m_frame_width + x) * 4;
			return {
				static_cast<matrix_type>(m_frame_buffer[offset + 0]),
				static_cast<matrix_type>(m_frame_buffer[offset + 1]),
				static_cast<matrix_type>(m_frame_buffer[offset + 2]),
				static_cast<matrix_type>(m_frame_buffer[offset + 3])
			};
		} else {
			return input;
		}
	}

private:
	std::unique_ptr<bbb::webrtc::video_session> m_session;
	std::vector<uint8_t> m_frame_buffer;
	int m_frame_width{0};
	int m_frame_height{0};
	std::atomic<bool> m_has_frame{false};
	std::atomic<bool> m_connected{false};
	std::mutex m_frame_mutex;

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
		return cfg;
	}

	void set_remote_offer(const std::string &sdp) {
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

		m_session->on_video_frame([this](const uint8_t *rgba, int width, int height) {
			std::lock_guard<std::mutex> lock(m_frame_mutex);
			m_frame_buffer.resize(static_cast<std::size_t>(width * height * 4));
			std::memcpy(m_frame_buffer.data(), rgba, static_cast<std::size_t>(width * height * 4));
			m_frame_width = width;
			m_frame_height = height;
			m_has_frame.store(true, std::memory_order_release);
		});

		m_session->set_remote_description("offer", sdp);
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
		{
			std::lock_guard<std::mutex> lock(m_frame_mutex);
			m_frame_buffer.clear();
			m_frame_width = 0;
			m_frame_height = 0;
			m_has_frame.store(false, std::memory_order_release);
		}
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

MIN_EXTERNAL(bbb_webrtc_recv_video);
