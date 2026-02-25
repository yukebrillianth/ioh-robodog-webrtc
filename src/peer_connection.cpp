#include "peer_connection.hpp"
#include <spdlog/spdlog.h>
#include <random>

namespace ss {

std::atomic<uint32_t> PeerConnection::next_ssrc_{42};

PeerConnection::PeerConnection(const std::string& peer_id,
                               const WebRtcConfig& config,
                               SignalingCallback signaling_cb)
    : peer_id_(peer_id)
    , config_(config)
    , signaling_cb_(std::move(signaling_cb))
    , ssrc_(next_ssrc_.fetch_add(1))
{
    setup_connection();
}

PeerConnection::~PeerConnection() {
    if (pc_) {
        pc_->close();
    }
}

void PeerConnection::setup_connection() {
    rtc::Configuration rtc_config;

    // STUN server
    if (!config_.stun_server.empty()) {
        rtc_config.iceServers.emplace_back(config_.stun_server);
        spdlog::debug("[{}] STUN: {}", peer_id_, config_.stun_server);
    }

    // TURN server (Cloudflare or custom)
    if (!config_.turn_server.empty()) {
        rtc::IceServer turn_server(config_.turn_server);
        turn_server.username = config_.turn_username;
        turn_server.password = config_.turn_credential;
        rtc_config.iceServers.push_back(turn_server);
        spdlog::debug("[{}] TURN: {}", peer_id_, config_.turn_server);
    }

    // Disable auto-negotiation — we manually trigger offer creation
    rtc_config.disableAutoNegotiation = true;

    pc_ = std::make_shared<rtc::PeerConnection>(rtc_config);

    // ─── Send local description (offer) to browser via signaling ─────────
    pc_->onLocalDescription([this](rtc::Description description) {
        std::string sdp = std::string(description);
        std::string type = description.typeString();
        spdlog::debug("[{}] Local description: {}", peer_id_, type);
        if (signaling_cb_) {
            signaling_cb_(type, sdp);
        }
    });

    // State change callback
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        std::string state_str;
        switch (state) {
            case rtc::PeerConnection::State::New: state_str = "new"; break;
            case rtc::PeerConnection::State::Connecting: state_str = "connecting"; break;
            case rtc::PeerConnection::State::Connected: state_str = "connected"; break;
            case rtc::PeerConnection::State::Disconnected: state_str = "disconnected"; break;
            case rtc::PeerConnection::State::Failed: state_str = "failed"; break;
            case rtc::PeerConnection::State::Closed: state_str = "closed"; break;
        }
        spdlog::info("[{}] Connection state: {}", peer_id_, state_str);

        connected_.store(state == rtc::PeerConnection::State::Connected);
        if (state == rtc::PeerConnection::State::Closed ||
            state == rtc::PeerConnection::State::Failed) {
            closed_.store(true);
        }

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.state = state_str;
        }
    });

    // ICE candidate callback → send to remote peer
    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        spdlog::debug("[{}] Local ICE candidate: {}", peer_id_, std::string(candidate));
        if (signaling_cb_) {
            std::string mid = candidate.mid();
            signaling_cb_("candidate",
                "{\"candidate\":\"" + std::string(candidate) + "\","
                "\"sdpMid\":\"" + mid + "\"}");
        }
    });

    pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            spdlog::info("[{}] ICE gathering complete", peer_id_);
        }
    });

    // ─── Add video track ─────────────────────────────────────────────────
    const std::string cname = "video-stream";
    const std::string msid = "stream-server";

    rtc::Description::Video media(cname, rtc::Description::Direction::SendOnly);
    media.addH264Codec(config_.video.payload_type);
    media.addSSRC(ssrc_, cname, msid, cname);
    media.setBitrate(config_.video.bitrate_kbps);

    video_track_ = pc_->addTrack(media);

    // Configure RTP packetizer chain:
    //   H264RtpPacketizer → RtcpSrReporter → RtcpNackResponder
    rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc_,
        cname,
        config_.video.payload_type,
        rtc::H264RtpPacketizer::defaultClockRate
    );

    // H.264 packetizer — LongStartSequence for byte-stream NALUs from GStreamer
    packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence,
        rtp_config_
    );

    // RTCP Sender Report
    sr_reporter_ = std::make_shared<rtc::RtcpSrReporter>(rtp_config_);
    packetizer_->addToChain(sr_reporter_);

    // RTCP NACK responder
    auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer_->addToChain(nack_responder);

    // Set the full media handler chain on the track
    video_track_->setMediaHandler(packetizer_);

    video_track_->onOpen([this]() {
        spdlog::info("[{}] Video track opened", peer_id_);
        needs_keyframe_.store(true);
    });

    video_track_->onClosed([this]() {
        spdlog::info("[{}] Video track closed", peer_id_);
    });

    spdlog::info("[{}] Peer connection created (SSRC={})", peer_id_, ssrc_);
}

void PeerConnection::start_offer() {
    // Server creates the offer (since it has sendonly tracks)
    pc_->setLocalDescription(rtc::Description::Type::Offer);
    spdlog::info("[{}] Created and sent SDP offer", peer_id_);
}

void PeerConnection::handle_answer(const std::string& sdp) {
    spdlog::debug("[{}] Received SDP answer", peer_id_);
    rtc::Description answer(sdp, rtc::Description::Type::Answer);
    pc_->setRemoteDescription(answer);
    needs_keyframe_.store(true);
}

void PeerConnection::handle_candidate(const std::string& candidate, const std::string& mid) {
    try {
        pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
        spdlog::debug("[{}] Added remote ICE candidate", peer_id_);
    } catch (const std::exception& e) {
        spdlog::warn("[{}] Failed to add ICE candidate: {}", peer_id_, e.what());
    }
}

void PeerConnection::send_h264_nal(const uint8_t* data, size_t size, uint64_t timestamp_us) {
    if (!connected_.load() || !video_track_ || !video_track_->isOpen()) {
        return;
    }

    try {
        // Update RTP timestamp (90kHz clock)
        rtp_config_->timestamp = static_cast<uint32_t>(
            (timestamp_us * rtc::H264RtpPacketizer::defaultClockRate) / 1'000'000);

        // Send the NAL unit(s) via the track
        auto byte_ptr = reinterpret_cast<const std::byte*>(data);
        video_track_->send(byte_ptr, size);

        // Update stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.rtp_packets_sent++;
            stats_.bytes_sent += size;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[{}] Failed to send RTP: {}", peer_id_, e.what());
    }
}

bool PeerConnection::is_connected() const {
    return connected_.load();
}

bool PeerConnection::is_closed() const {
    return closed_.load();
}

PeerConnection::Stats PeerConnection::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace ss
