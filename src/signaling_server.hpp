#pragma once

#include "config.hpp"
#include "webrtc_server.hpp"
#include <rtc/rtc.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <atomic>

namespace ss {

class SignalingServer {
public:
    SignalingServer(const AppConfig& config, WebRtcServer& webrtc_server);
    ~SignalingServer();

    // Non-copyable
    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    // Start / stop
    bool start();
    void stop();

    bool is_running() const { return running_.load(); }

    // Set callback for adaptive bitrate requests from clients
    using BitrateCallback = std::function<void(int bitrate_kbps)>;
    void set_bitrate_callback(BitrateCallback cb) { bitrate_cb_ = std::move(cb); }

private:
    void on_client_connected(std::shared_ptr<rtc::WebSocket> ws);
    void on_client_message(const std::string& peer_id,
                           std::shared_ptr<rtc::WebSocket> ws,
                           const std::string& message);
    void on_client_disconnected(const std::string& peer_id);

    // Send JSON message to a WebSocket
    void send_json(std::shared_ptr<rtc::WebSocket> ws,
                   const std::string& type,
                   const std::string& peer_id,
                   const std::string& payload);

    AppConfig config_;
    WebRtcServer& webrtc_server_;
    std::shared_ptr<rtc::WebSocketServer> ws_server_;

    struct ClientSession {
        std::shared_ptr<rtc::WebSocket> ws;
        std::string peer_id;
    };

    std::mutex clients_mutex_;
    std::unordered_map<std::string, ClientSession> clients_; // peer_id → session

    std::atomic<bool> running_{false};
    BitrateCallback bitrate_cb_;
};

} // namespace ss
