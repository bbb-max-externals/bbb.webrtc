#include "bbb/webrtc_session.hpp"
#include "bbb/opus_codec.hpp"
#include "bbb/audio_ring_buffer.hpp"

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>

namespace bbb {
namespace webrtc {

struct session::opus_encoder {
	bbb::webrtc::opus_encoder enc;
	explicit opus_encoder(const session_config &cfg)
		: enc(cfg.opus_sample_rate, cfg.opus_channels, cfg.opus_bitrate, cfg.opus_frame_size) {}
};

struct session::opus_decoder {
	bbb::webrtc::opus_decoder dec;
	explicit opus_decoder(const session_config &cfg)
		: dec(cfg.opus_sample_rate, cfg.opus_channels, cfg.opus_frame_size) {}
};

session::session(const session_config &config)
	: config_(config)
	, encoder_(std::make_unique<opus_encoder>(config))
	, decoder_(std::make_unique<opus_decoder>(config)) {}

session::~session() {
	close();
}

void session::setup_peer_connection() {
	rtc::Configuration rtc_config;
	for(const auto &srv : config_.ice_servers) {
		rtc_config.iceServers.push_back(rtc::IceServer(srv.url));
	}

	pc_ = std::make_shared<rtc::PeerConnection>(rtc_config);

	pc_->onStateChange([this](rtc::PeerConnection::State pc_state) {
		session::state s = session::state::disconnected;
		switch(pc_state) {
			case rtc::PeerConnection::State::New: s = session::state::disconnected; break;
			case rtc::PeerConnection::State::Connecting: s = session::state::connecting; break;
			case rtc::PeerConnection::State::Connected: s = session::state::connected; break;
			case rtc::PeerConnection::State::Disconnected: s = session::state::disconnected; break;
			case rtc::PeerConnection::State::Failed: s = session::state::failed; break;
			case rtc::PeerConnection::State::Closed: s = session::state::closed; break;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			state_ = s;
		}
		if(state_callback_) {
			state_callback_(s);
		}
	});

	pc_->onLocalDescription([this](const rtc::Description &desc) {
		if(sdp_callback_) {
			sdp_callback_(desc.typeString(), std::string(desc));
		}
	});

	pc_->onLocalCandidate([this](const rtc::Candidate &cand) {
		if(ice_callback_) {
			ice_callback_(std::string(cand), cand.mid());
		}
	});

	pc_->onTrack([this](rtc::shared_ptr<rtc::Track> track) {
		setup_track_callbacks(track);
	});
}

void session::add_audio_track() {
	if(!pc_) return;

	rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
	audio.addOpusCodec(111);
	audio.addSSRC(1, "audio-send");

	audio_track_ = pc_->addTrack(audio);

	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(1, "audio-send", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
	auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
	packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
	packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));
	audio_track_->setMediaHandler(packetizer);

	setup_track_callbacks(audio_track_);
}

void session::setup_track_callbacks(rtc::shared_ptr<rtc::Track> track) {
	auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
	track->setMediaHandler(depacketizer);

	track->onMessage([this](rtc::message_variant data) {
		if(!std::holds_alternative<rtc::binary>(data)) return;

		const auto &bin = std::get<rtc::binary>(data);
		if(bin.empty()) return;

		const int max_frames = config_.opus_frame_size;
		std::vector<float> pcm(max_frames * config_.opus_channels);

		std::vector<std::uint8_t> raw_bytes(bin.size());
		for(std::size_t i = 0; i < bin.size(); ++i) {
			raw_bytes[i] = static_cast<std::uint8_t>(bin[i]);
		}

		int decoded = decoder_->dec.decode(
			raw_bytes.data(),
			static_cast<int>(raw_bytes.size()),
			pcm.data(),
			max_frames
		);

		if(decoded > 0 && audio_received_callback_) {
			audio_received_callback_(pcm.data(), decoded, config_.opus_channels);
		}
	});
}

void session::create_offer() {
	std::lock_guard<std::mutex> lock(mutex_);
	if(!pc_) setup_peer_connection();
	add_audio_track();
	pc_->setLocalDescription();
}

void session::set_remote_description(const std::string &type, const std::string &sdp) {
	std::lock_guard<std::mutex> lock(mutex_);
	if(!pc_) setup_peer_connection();

	rtc::Description desc(sdp, type);

	if(type == "offer" && !audio_track_) {
		add_audio_track();
	}

	pc_->setRemoteDescription(desc);
}

void session::add_ice_candidate(const std::string &candidate, const std::string &mid) {
	std::lock_guard<std::mutex> lock(mutex_);
	if(!pc_) return;

	rtc::Candidate cand(candidate, mid);
	pc_->addRemoteCandidate(cand);
}

void session::send_audio(const float *samples, int frame_count, int channels) {
	if(!audio_track_ || !audio_track_->isOpen()) return;
	if(!encoder_) return;

	const int max_output = 4000;
	std::vector<std::uint8_t> output(max_output);

	int encoded = encoder_->enc.encode(samples, frame_count, output.data(), max_output);
	if(encoded > 0) {
		rtc::binary data(encoded);
		for(int i = 0; i < encoded; ++i) {
			data[i] = static_cast<std::byte>(output[i]);
		}
		audio_track_->send(data);
	}
}

int session::receive_audio(float *samples, int frame_count, int channels) {
	return 0;
}

void session::on_state_change(state_callback callback) {
	state_callback_ = std::move(callback);
}

void session::on_local_description(sdp_callback callback) {
	sdp_callback_ = std::move(callback);
}

void session::on_local_ice_candidate(ice_callback callback) {
	ice_callback_ = std::move(callback);
}

void session::on_audio_received(std::function<void(const float *data, int frame_count, int channels)> callback) {
	audio_received_callback_ = std::move(callback);
}

session::state session::get_state() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return state_;
}

void session::close() {
	std::lock_guard<std::mutex> lock(mutex_);
	if(audio_track_) {
		audio_track_->close();
		audio_track_.reset();
	}
	if(pc_) {
		pc_->close();
		pc_.reset();
	}
	state_ = session::state::closed;
}

}
}
