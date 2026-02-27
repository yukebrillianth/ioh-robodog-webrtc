#include "config.hpp"
#include "logger.hpp"
#include "rtsp_pipeline.hpp"
#include "webrtc_server.hpp"
#include "signaling_server.hpp"
#include "http_server.hpp"

#include <spdlog/spdlog.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>

// ─── Global shutdown flag ─────────────────────────────────────────────────────
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    spdlog::info("Received signal {} — shutting down gracefully...", sig);
    g_shutdown.store(true);
}

static void print_banner(const ss::AppConfig& cfg) {
    std::cout << R"(
  ┌─────────────────────────────────────────────┐
  │       STREAM SERVER v)" APP_VERSION R"(               │
  │       WebRTC Streaming for Robot Dog        │
  │       NVIDIA Jetson Orin NX Optimized       │
  └─────────────────────────────────────────────┘
)" << std::endl;

    spdlog::info("Configuration:");
    spdlog::info("  Signaling port  : {}", cfg.server.signaling_port);
    spdlog::info("  RTSP URL        : {}", cfg.rtsp.url.empty() ? "(test mode)" : cfg.rtsp.url);
    spdlog::info("  Transport       : {}", cfg.rtsp.transport);
    spdlog::info("  Codec           : {}", cfg.webrtc.video.codec);
    spdlog::info("  Bitrate         : {} kbps (max: {} kbps)",
                 cfg.webrtc.video.bitrate_kbps, cfg.webrtc.video.max_bitrate_kbps);
    spdlog::info("  Max peers       : {}", cfg.webrtc.max_peers);
    spdlog::info("  STUN            : {}", cfg.webrtc.stun_server);
    spdlog::info("  TURN            : {}", cfg.webrtc.turn_server.empty() ? "(disabled)" : cfg.webrtc.turn_server);
    spdlog::info("  HW encode       : {}", cfg.encoding.hw_encode ? "yes (Jetson)" : "no (software)");
    spdlog::info("  Passthrough     : {}", cfg.encoding.passthrough ? "yes" : "no");
    spdlog::info("  HTTP port       : {}", cfg.server.http_port);
    spdlog::info("  Web root        : {}", cfg.server.web_root);
}

int main(int argc, char* argv[]) {
    // ─── Parse arguments ──────────────────────────────────────────────────────
    std::string config_path = "config.yaml";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: stream-server [options]\n"
                      << "Options:\n"
                      << "  -c, --config <path>    Config file (default: config.yaml)\n"
                      << "  -h, --help             Show this help\n"
                      << "\nEnvironment variables:\n"
                      << "  RTSP_URL               RTSP camera URL\n"
                      << "  SIGNALING_PORT         WebSocket signaling port\n"
                      << "  STUN_SERVER            STUN server URL\n"
                      << "  TURN_SERVER            TURN server URL\n"
                      << "  TURN_USERNAME          TURN username\n"
                      << "  TURN_CREDENTIAL        TURN credential\n"
                      << "  VIDEO_BITRATE_KBPS     Video bitrate in kbps\n"
                      << "  VIDEO_MAX_BITRATE_KBPS Max video bitrate in kbps\n"
                      << "  LOG_LEVEL              Log level (trace/debug/info/warn/error)\n";
            return 0;
        }
    }

    // ─── Load configuration ───────────────────────────────────────────────────
    ss::AppConfig config;
    try {
        config = ss::load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    // ─── Initialize logger ────────────────────────────────────────────────────
    ss::init_logger(config.logging);
    print_banner(config);

    // ─── Signal handling ──────────────────────────────────────────────────────
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ─── Create components ────────────────────────────────────────────────────
    ss::WebRtcServer webrtc_server(config);
    ss::SignalingServer signaling_server(config, webrtc_server);
    ss::RtspPipeline rtsp_pipeline(config);
    ss::HttpServer http_server(config.server.http_port, config.server.web_root);

    // ─── Wire RTSP → WebRTC ───────────────────────────────────────────────────
    rtsp_pipeline.set_nal_callback(
        [&webrtc_server](const uint8_t* data, size_t size, uint64_t timestamp_us) {
            webrtc_server.broadcast_nal(data, size, timestamp_us);
        }
    );

    // Wire browser ABR → encoder bitrate control
    signaling_server.set_bitrate_callback(
        [&rtsp_pipeline](int bitrate_kbps) {
            rtsp_pipeline.set_bitrate(bitrate_kbps);
        }
    );

    // ─── Start everything ─────────────────────────────────────────────────────
    webrtc_server.start();

    if (!signaling_server.start()) {
        spdlog::critical("Failed to start signaling server");
        return 1;
    }

    if (!rtsp_pipeline.start()) {
        spdlog::critical("Failed to start RTSP pipeline");
        return 1;
    }

    if (!http_server.start()) {
        spdlog::warn("Failed to start HTTP server on port {} — web viewer unavailable",
                     config.server.http_port);
    }

    spdlog::info("All systems operational");
    spdlog::info("  WebSocket signaling : ws://0.0.0.0:{}", config.server.signaling_port);
    spdlog::info("  Web viewer (debug)  : http://0.0.0.0:{}/", config.server.http_port);
    spdlog::info("  Web viewer (embed)  : http://0.0.0.0:{}/embed.html", config.server.http_port);

    // ─── Main watchdog loop ───────────────────────────────────────────────────
    auto last_stats_time = std::chrono::steady_clock::now();
    constexpr auto stats_interval = std::chrono::seconds(10);

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Periodic stats logging
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= stats_interval) {
            last_stats_time = now;

            auto pipeline_stats = rtsp_pipeline.get_stats();
            auto webrtc_stats = webrtc_server.get_stats();

            spdlog::info("──── Health Check ────");
            spdlog::info("  Pipeline   : {} | Frames: {} | Bytes: {:.1f} MB | Reconnects: {}",
                        pipeline_stats.connected ? "CONNECTED" : "DISCONNECTED",
                        pipeline_stats.frames_received,
                        pipeline_stats.bytes_received / (1024.0 * 1024.0),
                        pipeline_stats.reconnect_count);
            spdlog::info("  WebRTC     : {}/{} peers connected | Sent: {:.1f} MB",
                        webrtc_stats.connected_peers,
                        webrtc_stats.total_peers,
                        webrtc_stats.total_bytes_sent / (1024.0 * 1024.0));
            spdlog::info("──────────────────────");

            // Watchdog: check if pipeline is healthy
            if (!rtsp_pipeline.is_running() && !g_shutdown.load()) {
                spdlog::warn("Pipeline not running! Attempting restart...");
                rtsp_pipeline.stop();
                rtsp_pipeline.start();
            }
        }
    }

    // ─── Graceful shutdown ────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    rtsp_pipeline.stop();
    http_server.stop();
    signaling_server.stop();
    webrtc_server.stop();
    spdlog::info("Shutdown complete. Goodbye!");

    return 0;
}
