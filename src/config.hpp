#pragma once

#include <string>
#include <cstdint>

namespace ss {

struct ServerConfig {
    uint16_t signaling_port = 8080;
    uint16_t http_port = 8081;
    std::string web_root = "./web";
};

struct RtspConfig {
    std::string url;
    std::string transport = "tcp";  // tcp or udp
    int latency_ms = 0;
    int reconnect_interval_ms = 3000;
    int reconnect_max_attempts = 0; // 0 = unlimited
};

struct VideoConfig {
    std::string codec = "H264";
    int clock_rate = 90000;
    int payload_type = 96;
    int bitrate_kbps = 4000;
    int max_bitrate_kbps = 8000;
    int min_bitrate_kbps = 500;
    int fps = 30;
};

struct WebRtcConfig {
    std::string stun_server = "stun:stun.cloudflare.com:3478";
    std::string turn_server;
    std::string turn_username;
    std::string turn_credential;
    int max_peers = 4;
    VideoConfig video;
};

struct EncodingConfig {
    bool hw_encode = false;
    bool passthrough = true;
    std::string preset = "UltraFastPreset";
    int idr_interval = 30;
    bool insert_sps_pps = true;
};

struct LoggingConfig {
    std::string level = "info";
    std::string file;
    int max_file_size_mb = 10;
    int max_files = 3;
};

struct AppConfig {
    ServerConfig server;
    RtspConfig rtsp;
    WebRtcConfig webrtc;
    EncodingConfig encoding;
    LoggingConfig logging;
};

// Load configuration from YAML file, with environment variable overrides
AppConfig load_config(const std::string& path);

} // namespace ss
