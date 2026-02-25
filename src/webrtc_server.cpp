#include "webrtc_server.hpp"
#include <spdlog/spdlog.h>
#include <random>
#include <sstream>
#include <iomanip>

namespace ss {

static std::string generate_peer_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;

    std::ostringstream oss;
    oss << "peer-" << std::hex << std::setfill('0') << std::setw(8) << dist(gen);
    return oss.str();
}

WebRtcServer::WebRtcServer(const AppConfig& config) : config_(config) {}

WebRtcServer::~WebRtcServer() {
    stop();
}

std::string WebRtcServer::create_peer(SignalingCallback signaling_cb) {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    // Check max peer limit
    if (static_cast<int>(peers_.size()) >= config_.webrtc.max_peers) {
        spdlog::warn("Max peers ({}) reached, rejecting new connection", config_.webrtc.max_peers);
        return "";
    }

    std::string peer_id = generate_peer_id();

    try {
        auto peer = std::make_shared<PeerConnection>(
            peer_id, config_.webrtc, std::move(signaling_cb));
        peers_[peer_id] = peer;
        spdlog::info("Created peer: {} (total: {})", peer_id, peers_.size());
        return peer_id;
    } catch (const std::exception& e) {
        spdlog::error("Failed to create peer: {}", e.what());
        return "";
    }
}

void WebRtcServer::start_offer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second->start_offer();
    } else {
        spdlog::warn("Unknown peer for offer: {}", peer_id);
    }
}

void WebRtcServer::handle_answer(const std::string& peer_id, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second->handle_answer(sdp);
    } else {
        spdlog::warn("Unknown peer for answer: {}", peer_id);
    }
}

void WebRtcServer::handle_candidate(const std::string& peer_id,
                                     const std::string& candidate,
                                     const std::string& mid) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second->handle_candidate(candidate, mid);
    } else {
        spdlog::warn("Unknown peer for candidate: {}", peer_id);
    }
}

void WebRtcServer::remove_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        peers_.erase(it);
        spdlog::info("Removed peer: {} (remaining: {})", peer_id, peers_.size());
    }
}

void WebRtcServer::broadcast_nal(const uint8_t* data, size_t size, uint64_t timestamp_us) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto& [id, peer] : peers_) {
        if (peer->is_connected()) {
            peer->send_h264_nal(data, size, timestamp_us);
        }
    }
}

void WebRtcServer::start() {
    running_.store(true);
    cleanup_thread_ = std::thread(&WebRtcServer::cleanup_loop, this);
    spdlog::info("WebRTC server started (max peers: {})", config_.webrtc.max_peers);
}

void WebRtcServer::stop() {
    running_.store(false);
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }

    // Close all peers
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_.clear();
    spdlog::info("WebRTC server stopped");
}

size_t WebRtcServer::peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_.size();
}

WebRtcServer::ServerStats WebRtcServer::get_stats() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    ServerStats stats;
    stats.total_peers = peers_.size();
    for (auto& [id, peer] : peers_) {
        if (peer->is_connected()) {
            stats.connected_peers++;
        }
        auto ps = peer->get_stats();
        stats.total_bytes_sent += ps.bytes_sent;
    }
    return stats;
}

void WebRtcServer::cleanup_loop() {
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (auto it = peers_.begin(); it != peers_.end();) {
                if (it->second->is_closed()) {
                    spdlog::info("Cleaning up disconnected peer: {}", it->first);
                    it = peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Check every 2 seconds
        for (int i = 0; i < 20 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace ss
