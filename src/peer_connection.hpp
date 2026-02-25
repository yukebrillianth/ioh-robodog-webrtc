#pragma once

#include "config.hpp"
#include <rtc/rtc.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

namespace ss {

// Callback for signaling messages back to client
using SignalingCallback = std::function<void(const std::string& type, const std::string& payload)>;

class PeerConnection {
public:
    PeerConnection(const std::string& peer_id,
                   const WebRtcConfig& config,
                   SignalingCallback signaling_cb);
    ~PeerConnection();

    // Non-copyable
    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    // Server creates offer → sends to browser
    void start_offer();

    // Browser sends answer back → server sets remote description
    void handle_answer(const std::string& sdp);

    // ICE candidate exchange
    void handle_candidate(const std::string& candidate, const std::string& mid);

    // Send H.264 NAL units to remote peer
    void send_h264_nal(const uint8_t* data, size_t size, uint64_t timestamp_us);

    // Request a keyframe (for new connections)
    bool needs_keyframe() const { return needs_keyframe_.load(); }
    void keyframe_sent() { needs_keyframe_.store(false); }

    // Connection state
    bool is_connected() const;
    bool is_closed() const;
    std::string id() const { return peer_id_; }

    // Stats
    struct Stats {
        uint64_t rtp_packets_sent = 0;
        uint64_t bytes_sent = 0;
        std::string state = "new";
    };
    Stats get_stats() const;

private:
    void setup_connection();

    std::string peer_id_;
    WebRtcConfig config_;
    SignalingCallback signaling_cb_;

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config_;
    std::shared_ptr<rtc::H264RtpPacketizer> packetizer_;
    std::shared_ptr<rtc::RtcpSrReporter> sr_reporter_;

    std::atomic<bool> needs_keyframe_{true};
    std::atomic<bool> connected_{false};
    std::atomic<bool> closed_{false};

    mutable std::mutex stats_mutex_;
    Stats stats_;

    uint32_t ssrc_;
    static std::atomic<uint32_t> next_ssrc_;
};

} // namespace ss
