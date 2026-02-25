#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <stdexcept>

namespace ss {

static std::string env_or(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

static int env_int_or(const char* name, int fallback) {
    const char* val = std::getenv(name);
    return val ? std::stoi(val) : fallback;
}

AppConfig load_config(const std::string& path) {
    AppConfig cfg;
    YAML::Node root;

    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load config: " + std::string(e.what()));
    }

    // Server
    if (auto s = root["server"]) {
        cfg.server.signaling_port = s["signaling_port"].as<uint16_t>(cfg.server.signaling_port);
        cfg.server.web_root = s["web_root"].as<std::string>(cfg.server.web_root);
    }

    // RTSP
    if (auto r = root["rtsp"]) {
        cfg.rtsp.url = r["url"].as<std::string>("");
        cfg.rtsp.transport = r["transport"].as<std::string>(cfg.rtsp.transport);
        cfg.rtsp.latency_ms = r["latency_ms"].as<int>(cfg.rtsp.latency_ms);
        cfg.rtsp.reconnect_interval_ms = r["reconnect_interval_ms"].as<int>(cfg.rtsp.reconnect_interval_ms);
        cfg.rtsp.reconnect_max_attempts = r["reconnect_max_attempts"].as<int>(cfg.rtsp.reconnect_max_attempts);
    }

    // WebRTC
    if (auto w = root["webrtc"]) {
        cfg.webrtc.stun_server = w["stun_server"].as<std::string>(cfg.webrtc.stun_server);
        cfg.webrtc.turn_server = w["turn_server"].as<std::string>("");
        cfg.webrtc.turn_username = w["turn_username"].as<std::string>("");
        cfg.webrtc.turn_credential = w["turn_credential"].as<std::string>("");
        cfg.webrtc.max_peers = w["max_peers"].as<int>(cfg.webrtc.max_peers);

        if (auto v = w["video"]) {
            cfg.webrtc.video.codec = v["codec"].as<std::string>(cfg.webrtc.video.codec);
            cfg.webrtc.video.clock_rate = v["clock_rate"].as<int>(cfg.webrtc.video.clock_rate);
            cfg.webrtc.video.payload_type = v["payload_type"].as<int>(cfg.webrtc.video.payload_type);
            cfg.webrtc.video.bitrate_kbps = v["bitrate_kbps"].as<int>(cfg.webrtc.video.bitrate_kbps);
            cfg.webrtc.video.max_bitrate_kbps = v["max_bitrate_kbps"].as<int>(cfg.webrtc.video.max_bitrate_kbps);
            cfg.webrtc.video.min_bitrate_kbps = v["min_bitrate_kbps"].as<int>(cfg.webrtc.video.min_bitrate_kbps);
            cfg.webrtc.video.fps = v["fps"].as<int>(cfg.webrtc.video.fps);
        }
    }

    // Encoding
    if (auto e = root["encoding"]) {
        cfg.encoding.hw_encode = e["hw_encode"].as<bool>(cfg.encoding.hw_encode);
        cfg.encoding.passthrough = e["passthrough"].as<bool>(cfg.encoding.passthrough);
        cfg.encoding.preset = e["preset"].as<std::string>(cfg.encoding.preset);
        cfg.encoding.idr_interval = e["idr_interval"].as<int>(cfg.encoding.idr_interval);
        cfg.encoding.insert_sps_pps = e["insert_sps_pps"].as<bool>(cfg.encoding.insert_sps_pps);
    }

    // Logging
    if (auto l = root["logging"]) {
        cfg.logging.level = l["level"].as<std::string>(cfg.logging.level);
        cfg.logging.file = l["file"].as<std::string>("");
        cfg.logging.max_file_size_mb = l["max_file_size_mb"].as<int>(cfg.logging.max_file_size_mb);
        cfg.logging.max_files = l["max_files"].as<int>(cfg.logging.max_files);
    }

    // Environment variable overrides (Docker / systemd)
    cfg.rtsp.url = env_or("RTSP_URL", cfg.rtsp.url);
    cfg.server.signaling_port = static_cast<uint16_t>(
        env_int_or("SIGNALING_PORT", cfg.server.signaling_port));
    cfg.webrtc.stun_server = env_or("STUN_SERVER", cfg.webrtc.stun_server);
    cfg.webrtc.turn_server = env_or("TURN_SERVER", cfg.webrtc.turn_server);
    cfg.webrtc.turn_username = env_or("TURN_USERNAME", cfg.webrtc.turn_username);
    cfg.webrtc.turn_credential = env_or("TURN_CREDENTIAL", cfg.webrtc.turn_credential);
    cfg.webrtc.video.bitrate_kbps = env_int_or("VIDEO_BITRATE_KBPS", cfg.webrtc.video.bitrate_kbps);
    cfg.webrtc.video.max_bitrate_kbps = env_int_or("VIDEO_MAX_BITRATE_KBPS", cfg.webrtc.video.max_bitrate_kbps);
    cfg.logging.level = env_or("LOG_LEVEL", cfg.logging.level);

    return cfg;
}

} // namespace ss
