#include "bbb/video_session.hpp"

#pragma push_macro("NIL")
#undef NIL
#include <rtc/rtc.hpp>
#pragma pop_macro("NIL")

#include <chrono>
#include <mutex>

namespace bbb {
namespace webrtc {

video_session::video_session(const video_session_config &config)
	: config_(config)
	, encoder_(create_video_encoder())
	, decoder_(create_video_decoder()) {

	encoder_->on_encoded([this](const encoded_frame &frame) {
		if(shutting_down_.load()) return;
		if(!video_track_ || !video_track_->isOpen()) return;

		rtc::binary data(frame.data.size());
		for(std::size_t i = 0; i < frame.data.size(); ++i) {
			data[i] = static_cast<std::byte>(frame.data[i]);
		}

		video_track_->send(data);
	});

	decoder_->on_decoded([this](const decoded_frame &frame) {
		if(shutting_down_.load()) return;
		if(video_frame_callback_) {
			video_frame_callback_(frame.data.data(), frame.width, frame.height);
		}
	});

	encoder_->init(config_.width, config_.height, config_.bitrate_bps, config_.fps);
	decoder_->init();
}

video_session::~video_session() {
	close();
}

void video_session::setup_peer_connection() {
	rtc::Configuration rtc_config;
	for(const auto &srv : config_.ice_servers) {
		rtc_config.iceServers.emplace_back(srv.url);
	}

	pc_ = std::make_shared<rtc::PeerConnection>(rtc_config);

	pc_->onStateChange([this](rtc::PeerConnection::State pc_state) {
		if(shutting_down_.load()) return;
		video_session::state s = video_session::state::disconnected;
		switch(pc_state) {
			case rtc::PeerConnection::State::New: s = video_session::state::disconnected; break;
			case rtc::PeerConnection::State::Connecting: s = video_session::state::connecting; break;
			case rtc::PeerConnection::State::Connected: s = video_session::state::connected; break;
			case rtc::PeerConnection::State::Disconnected: s = video_session::state::disconnected; break;
			case rtc::PeerConnection::State::Failed: s = video_session::state::failed; break;
			case rtc::PeerConnection::State::Closed: s = video_session::state::closed; break;
		}
		state_.store(s);
		if(state_callback_) {
			state_callback_(s);
		}
	});

	pc_->onLocalDescription([this](const rtc::Description &desc) {
		if(shutting_down_.load()) return;
		if(sdp_callback_) {
			sdp_callback_(desc.typeString(), std::string(desc));
		}
	});

	pc_->onLocalCandidate([this](const rtc::Candidate &cand) {
		if(shutting_down_.load()) return;
		if(ice_callback_) {
			ice_callback_(std::string(cand), cand.mid());
		}
	});

	pc_->onTrack([this](rtc::shared_ptr<rtc::Track> track) {
		if(shutting_down_.load()) return;
		setup_video_track_callbacks(track);
	});
}

void video_session::add_video_track() {
	if(!pc_) return;

	rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
	video.addH264Codec(96);
	video.addSSRC(42, "video-send");

	video_track_ = pc_->addTrack(video);

	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(42, "video-send", 96, 90000);
	rtp_config_ = rtpConfig;

	auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
		rtc::NalUnit::Separator::StartSequence, rtpConfig);
	packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());

	video_track_->setMediaHandler(packetizer);
}

void video_session::setup_video_track_callbacks(rtc::shared_ptr<rtc::Track> track) {
	auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
	track->setMediaHandler(depacketizer);

	track->onMessage([this](rtc::message_variant data) {
		if(shutting_down_.load()) return;
		if(!std::holds_alternative<rtc::binary>(data)) return;

		const auto &bin = std::get<rtc::binary>(data);
		if(bin.empty()) return;

		std::vector<uint8_t> nal(bin.size());
		for(std::size_t i = 0; i < bin.size(); ++i) {
			nal[i] = static_cast<uint8_t>(bin[i]);
		}

		uint64_t ts_us = 0;
		if(rtp_config_) {
			ts_us = static_cast<uint64_t>(rtp_config_->timestamp) * 1000 / 90;
		}

		decoder_->decode(nal.data(), nal.size(), ts_us);
	});
}

void video_session::create_offer() {
	if(!pc_) setup_peer_connection();
	add_video_track();
	pc_->setLocalDescription();
}

void video_session::set_remote_description(const std::string &type, const std::string &sdp) {
	if(!pc_) setup_peer_connection();

	rtc::Description desc(sdp, type);

	if(type == "offer" && !video_track_) {
		add_video_track();
	}

	pc_->setRemoteDescription(desc);
}

void video_session::add_ice_candidate(const std::string &candidate, const std::string &mid) {
	if(!pc_) return;

	rtc::Candidate cand(candidate, mid);
	pc_->addRemoteCandidate(cand);
}

void video_session::send_video_frame(const uint8_t *rgba_data, int stride, int width, int height) {
	if(!video_track_ || !video_track_->isOpen()) return;
	if(!encoder_) return;

	uint64_t timestamp_us = 0;
	if(rtp_config_) {
		auto now = std::chrono::steady_clock::now();
		timestamp_us = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
		rtp_config_->timestamp = static_cast<uint32_t>(timestamp_us * 90 / 1000000);
	}

	encoder_->encode(rgba_data, stride, timestamp_us);
}

void video_session::request_keyframe() {
	if(encoder_) {
		encoder_->request_keyframe();
	}
}

void video_session::on_state_change(state_callback callback) {
	state_callback_ = std::move(callback);
}

void video_session::on_local_description(sdp_callback callback) {
	sdp_callback_ = std::move(callback);
}

void video_session::on_local_ice_candidate(ice_callback callback) {
	ice_callback_ = std::move(callback);
}

void video_session::on_video_frame(video_frame_callback callback) {
	video_frame_callback_ = std::move(callback);
}

video_session::state video_session::get_state() const {
	return state_.load();
}

void video_session::close() {
	shutting_down_.store(true);

	// Phase 1: Under lock — null callbacks and steal references
	std::shared_ptr<rtc::PeerConnection> pc_local;
	rtc::shared_ptr<rtc::Track> track_local;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		state_callback_ = nullptr;
		sdp_callback_ = nullptr;
		ice_callback_ = nullptr;
		video_frame_callback_ = nullptr;
		track_local = std::move(video_track_);
		rtp_config_.reset();
		pc_local = std::move(pc_);
		state_ = video_session::state::closed;
	}

	// Phase 2: Close outside lock to avoid deadlock with network callbacks.
	if(track_local) track_local->close();
	if(pc_local) pc_local->close();
}

} // namespace webrtc
} // namespace bbb
