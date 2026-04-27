#include "c74_min.h"
#include "bbb/webrtc_session.hpp"
#include "bbb/audio_ring_buffer.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace c74::min;

static std::string to_string(const symbol &s) {
	return std::string(s.c_str());
}

class bbb_webrtc_recv : public object<bbb_webrtc_recv>, public vector_operator<> {
public:
	MIN_DESCRIPTION{"Receive audio via WebRTC"};
	MIN_TAGS{"webrtc, audio, network"};
	MIN_AUTHOR{"ISHII 2bit"};

	outlet<> signal_out{this, "(signal) audio output"};
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

	bbb_webrtc_recv(const atoms &args = {}) {
		m_ring_buffer = std::make_unique<bbb::webrtc::audio_ring_buffer>(48000 * 2);
		m_init_timer.delay(0);
	}

	~bbb_webrtc_recv() {
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
			cout << "bbb.webrtc.recv status:" << endl;
			cout << "  state: " << state_string() << endl;
			cout << "  stun: " << to_string(stun_server) << endl;
			cout << "  buffered: " << static_cast<int>(m_ring_buffer->available_read()) << " samples" << endl;
			return {};
		}
	};

	void operator()(audio_bundle input, audio_bundle output) {
		auto frame_count = static_cast<int>(output.frame_count());
		auto *out = output.samples(0);

		if(!m_connected.load()) {
			for(int i = 0; i < frame_count; ++i) {
				out[i] = 0.0;
			}
			return;
		}

		std::vector<float> read_buf(frame_count);
		auto read = m_ring_buffer->read(read_buf.data(), frame_count);

		for(int i = 0; i < frame_count; ++i) {
			if(i < static_cast<int>(read)) {
				out[i] = static_cast<double>(read_buf[i]);
			} else {
				out[i] = 0.0;
			}
		}
	}

private:
	std::unique_ptr<bbb::webrtc::session> m_session;
	std::unique_ptr<bbb::webrtc::audio_ring_buffer> m_ring_buffer;
	std::atomic<bool> m_connected{false};

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

	bbb::webrtc::session_config make_config() {
		bbb::webrtc::session_config cfg;
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
		m_session = std::make_unique<bbb::webrtc::session>(cfg);

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

		m_session->on_state_change([this](bbb::webrtc::session::state state) {
			m_connected.store(state == bbb::webrtc::session::state::connected);
			std::lock_guard<std::mutex> lock(m_pending_mutex);
			m_pending_messages.push_back({"state", {state_to_string(state)}});
			m_message_queue.set();
		});

		m_session->on_audio_received([this](const float *data, int frame_count, int channels) {
			m_ring_buffer->write(data, frame_count * channels);
		});

		m_session->set_remote_description("offer", sdp);
	}

	void add_remote_ice(const std::string &candidate, const std::string &mid) {
		if(m_session) {
			m_session->add_ice_candidate(candidate, mid);
		}
	}

	void close() {
		m_connected.store(false);
		if(m_session) {
			m_session->close();
			m_session.reset();
		}
		m_ring_buffer->reset();
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

	static std::string state_to_string(bbb::webrtc::session::state state) {
		switch(state) {
			case bbb::webrtc::session::state::disconnected: return "disconnected";
			case bbb::webrtc::session::state::connecting: return "connecting";
			case bbb::webrtc::session::state::connected: return "connected";
			case bbb::webrtc::session::state::failed: return "failed";
			case bbb::webrtc::session::state::closed: return "closed";
		}
		return "unknown";
	}

	std::string state_string() const {
		if(!m_session) return "not initialized";
		return state_to_string(m_session->get_state());
	}
};

MIN_EXTERNAL(bbb_webrtc_recv);
