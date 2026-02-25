#pragma once

#include "config.hpp"
#include "peer_connection.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>
#include <atomic>

namespace ss {

class WebRtcServer {
public:
    explicit WebRtcServer(const AppConfig& config);
    ~WebRtcServer();

    // Non-copyable
    WebRtcServer(const WebRtcServer&) = delete;
    WebRtcServer& operator=(const WebRtcServer&) = delete;

    // Create a new peer connection, returns peer_id
    std::string create_peer(SignalingCallback signaling_cb);

    // Handle signaling messages for a peer
    void handle_offer(const std::string& peer_id, const std::string& sdp);
    void handle_candidate(const std::string& peer_id,
                          const std::string& candidate, const std::string& mid);

    // Remove a peer
    void remove_peer(const std::string& peer_id);

    // Broadcast H.264 NAL units to all connected peers
    void broadcast_nal(const uint8_t* data, size_t size, uint64_t timestamp_us);

    // Start cleanup loop (removes dead peers)
    void start();
    void stop();

    // Get connected peer count
    size_t peer_count() const;

    // Get all peer stats
    struct ServerStats {
        size_t total_peers = 0;
        size_t connected_peers = 0;
        uint64_t total_bytes_sent = 0;
    };
    ServerStats get_stats() const;

private:
    void cleanup_loop();

    AppConfig config_;
    mutable std::mutex peers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> peers_;

    std::thread cleanup_thread_;
    std::atomic<bool> running_{false};
};

} // namespace ss
