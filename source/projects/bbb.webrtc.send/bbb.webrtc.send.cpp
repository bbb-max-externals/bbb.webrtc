#include "c74_min.h"
#include "bbb/webrtc_session.hpp"
#include "bbb/audio_ring_buffer.hpp"
#include "bbb/audio_rate_converter.hpp"

#include <array>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace c74::min;

static std::string to_string(const symbol &s) {
	return std::string(s.c_str());
}

class bbb_webrtc_send : public object<bbb_webrtc_send>, public vector_operator<> {
public:
	MIN_DESCRIPTION{"Send audio via WebRTC"};
	MIN_TAGS{"webrtc, audio, network"};
	MIN_AUTHOR{"ISHII 2bit"};

	inlet<> signal_in{this, "(signal) audio input"};
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

	attribute<int> bitrate{this, "bitrate", 64000,
		description{"Opus bitrate in bps"},
		range{6000, 510000}
	};


	~bbb_webrtc_send() {
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
			cout << "bbb.webrtc.send status:" << endl;
			cout << "  state: " << state_string() << endl;
			cout << "  stun: " << to_string(stun_server) << endl;
			cout << "  bitrate: " << bitrate << endl;
			return {};
		}
	};

	message<> dspsetup{this, "dspsetup", MIN_FUNCTION {
		std::lock_guard<std::mutex> lock(m_dsp_lifecycle);
		const double rate = args[0];
		m_converter.setup(rate, 48000);
		if (!m_storage) {
			m_storage = std::make_unique<bbb::webrtc::audio_ring_buffer>(48000 * 2);
			m_fifo.store(m_storage.get());
		}
		return {};
	}};

	void operator()(audio_bundle input, audio_bundle) {
		std::unique_lock<std::mutex> lock(m_dsp_lifecycle, std::try_to_lock);
		if (!lock || !m_connected.load() || !m_storage || !input.channel_count()) return;
		int index = 0;
		auto read = [&](float& sample) {
			if (index == input.frame_count()) return false;
			double sum = 0;
			for (int ch = 0; ch < input.channel_count(); ++ch) sum += input.samples(ch)[index];
			sample = static_cast<float>(sum / input.channel_count());
			++index;
			return true;
		};
		float sample;
		while (m_converter.next(read, sample)) m_storage->write(&sample, 1);
	}

private:
	std::thread m_sender;
	std::atomic<bool> m_stop_sender{false};

	void send_frames() {
		std::array<float, 960> frame;
		while (!m_stop_sender.load()) {
			auto* fifo = m_fifo.load();
			if (fifo && fifo->available_read() >= frame.size()) {
				fifo->read(frame.data(), frame.size());
				try {
					m_session->send_audio(frame.data(), frame.size(), 1);
				} catch (const std::exception& e) {
					std::lock_guard<std::mutex> lock(m_pending_mutex);
					m_pending_messages.push_back({"error", {e.what()}});
					m_message_queue.set();
					return;
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
	}

	std::unique_ptr<bbb::webrtc::session> m_session;
	std::unique_ptr<bbb::webrtc::audio_ring_buffer> m_storage;
	// Published once at DSP setup; storage lives until the session is stopped.
	std::atomic<bbb::webrtc::audio_ring_buffer*> m_fifo{nullptr};
	std::mutex m_dsp_lifecycle;
	bbb::webrtc::audio_rate_converter m_converter;
	std::atomic<bool> m_connected{false};

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
		cfg.opus_bitrate = static_cast<int>(bitrate);
		return cfg;
	}

	void create_offer() {
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

		m_session->create_offer();
		m_stop_sender = false;
		m_sender = std::thread([this] { send_frames(); });
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
		m_stop_sender = true;
		if (m_sender.joinable()) m_sender.join();
		if(m_session) {
			m_session->close();
			m_session.reset();
		}
		std::lock_guard<std::mutex> lock(m_dsp_lifecycle);
		if (m_storage) m_storage->reset();
		m_converter.reset();
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

MIN_EXTERNAL(bbb_webrtc_send);
