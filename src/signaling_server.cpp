#include "signaling_server.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace ss {

SignalingServer::SignalingServer(const AppConfig& config, WebRtcServer& webrtc_server)
    : config_(config)
    , webrtc_server_(webrtc_server)
{
}

SignalingServer::~SignalingServer() {
    stop();
}

bool SignalingServer::start() {
    try {
        rtc::WebSocketServer::Configuration ws_config;
        ws_config.port = config_.server.signaling_port;
        ws_config.enableTls = false;

        ws_server_ = std::make_shared<rtc::WebSocketServer>(ws_config);

        ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
            on_client_connected(ws);
        });

        running_.store(true);
        spdlog::info("Signaling server listening on ws://0.0.0.0:{}", config_.server.signaling_port);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Failed to start signaling server: {}", e.what());
        return false;
    }
}

void SignalingServer::stop() {
    running_.store(false);

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [id, session] : clients_) {
            if (session.ws) {
                session.ws->close();
            }
        }
        clients_.clear();
    }

    if (ws_server_) {
        ws_server_->stop();
        ws_server_.reset();
    }

    spdlog::info("Signaling server stopped");
}

void SignalingServer::on_client_connected(std::shared_ptr<rtc::WebSocket> ws) {
    auto ws_weak = std::weak_ptr<rtc::WebSocket>(ws);

    // Signaling callback: sends offer/answer/candidate to the browser
    SignalingCallback sig_cb = [this, ws_weak](const std::string& type, const std::string& payload) {
        auto ws_shared = ws_weak.lock();
        if (ws_shared) {
            json msg;
            msg["type"] = type;

            if (type == "offer" || type == "answer") {
                msg["sdp"] = payload;
            } else if (type == "candidate") {
                try {
                    msg["data"] = json::parse(payload);
                } catch (...) {
                    msg["data"] = payload;
                }
            }

            try {
                ws_shared->send(msg.dump());
            } catch (const std::exception& e) {
                spdlog::warn("Failed to send signaling message: {}", e.what());
            }
        }
    };

    // Create WebRTC peer
    std::string peer_id = webrtc_server_.create_peer(std::move(sig_cb));

    if (peer_id.empty()) {
        spdlog::warn("Rejected client: max peers reached");
        json reject;
        reject["type"] = "error";
        reject["message"] = "Server full, max peers reached";
        try {
            ws->send(reject.dump());
            ws->close();
        } catch (...) {}
        return;
    }

    spdlog::info("Client connected, assigned peer: {}", peer_id);

    // Send welcome with peer ID and ICE server config
    json welcome;
    welcome["type"] = "welcome";
    welcome["peerId"] = peer_id;

    json ice_servers = json::array();
    if (!config_.webrtc.stun_server.empty()) {
        ice_servers.push_back({{"urls", config_.webrtc.stun_server}});
    }
    if (!config_.webrtc.turn_server.empty()) {
        json turn;
        turn["urls"] = config_.webrtc.turn_server;
        turn["username"] = config_.webrtc.turn_username;
        turn["credential"] = config_.webrtc.turn_credential;
        ice_servers.push_back(turn);
    }
    welcome["iceServers"] = ice_servers;

    try {
        ws->send(welcome.dump());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to send welcome: {}", e.what());
    }

    // Store session
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[peer_id] = ClientSession{ws, peer_id};
    }

    // Set up message handler
    std::string captured_peer_id = peer_id;
    ws->onMessage([this, captured_peer_id, ws](auto data) {
        if (std::holds_alternative<std::string>(data)) {
            on_client_message(captured_peer_id, ws, std::get<std::string>(data));
        }
    });

    ws->onClosed([this, captured_peer_id]() {
        on_client_disconnected(captured_peer_id);
    });

    ws->onError([this, captured_peer_id](std::string error) {
        spdlog::warn("[{}] WebSocket error: {}", captured_peer_id, error);
        on_client_disconnected(captured_peer_id);
    });

    // SERVER creates the offer (since it has sendonly video track)
    // The onLocalDescription callback will send it to the browser
    webrtc_server_.start_offer(peer_id);
}

void SignalingServer::on_client_message(const std::string& peer_id,
                                         std::shared_ptr<rtc::WebSocket> ws,
                                         const std::string& message) {
    try {
        auto msg = json::parse(message);
        std::string type = msg.value("type", "");

        if (type == "answer") {
            // Browser sends answer in response to our offer
            std::string sdp = msg.value("sdp", "");
            if (!sdp.empty()) {
                spdlog::debug("[{}] Received SDP answer", peer_id);
                webrtc_server_.handle_answer(peer_id, sdp);
            }
        } else if (type == "candidate") {
            auto data = msg.value("data", json::object());
            std::string candidate = data.value("candidate", "");
            std::string mid = data.value("sdpMid", "0");

            if (!candidate.empty()) {
                spdlog::debug("[{}] Received ICE candidate", peer_id);
                webrtc_server_.handle_candidate(peer_id, candidate, mid);
            }
        } else if (type == "ping") {
            json pong;
            pong["type"] = "pong";
            ws->send(pong.dump());
        } else {
            spdlog::debug("[{}] Unknown message type: {}", peer_id, type);
        }

    } catch (const json::exception& e) {
        spdlog::warn("[{}] Invalid JSON message: {}", peer_id, e.what());
    }
}

void SignalingServer::on_client_disconnected(const std::string& peer_id) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(peer_id);
    }

    webrtc_server_.remove_peer(peer_id);
    spdlog::info("Client disconnected: {}", peer_id);
}

void SignalingServer::send_json(std::shared_ptr<rtc::WebSocket> ws,
                                 const std::string& type,
                                 const std::string& peer_id,
                                 const std::string& payload) {
    json msg;
    msg["type"] = type;
    msg["peerId"] = peer_id;
    msg["data"] = payload;

    try {
        ws->send(msg.dump());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to send to client: {}", e.what());
    }
}

} // namespace ss
