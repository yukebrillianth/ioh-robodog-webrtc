#pragma once

#include "config.hpp"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace ss {

// Callback: receives H.264 NAL unit data (with start codes)
using NalUnitCallback = std::function<void(const uint8_t* data, size_t size, uint64_t timestamp_us)>;

class RtspPipeline {
public:
    explicit RtspPipeline(const AppConfig& config);
    ~RtspPipeline();

    // Non-copyable
    RtspPipeline(const RtspPipeline&) = delete;
    RtspPipeline& operator=(const RtspPipeline&) = delete;

    // Set callback for received NAL units
    void set_nal_callback(NalUnitCallback cb);

    // Start / stop the pipeline
    bool start();
    void stop();

    // Check if pipeline is running
    bool is_running() const { return running_.load(); }

    // Dynamically adjust encoder bitrate (only in re-encode mode)
    void set_bitrate(int bitrate_kbps);

    // Get pipeline statistics
    struct Stats {
        uint64_t frames_received = 0;
        uint64_t bytes_received = 0;
        uint64_t reconnect_count = 0;
        bool connected = false;
    };
    Stats get_stats() const;

private:
    void build_pipeline();
    void pipeline_thread();
    void handle_bus_message(GstMessage* msg);
    void attempt_reconnect();

    // GStreamer appsink callback
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);

    AppConfig config_;
    NalUnitCallback nal_callback_;

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstElement* encoder_ = nullptr;  // for dynamic bitrate control
    bool is_hw_encode_ = false;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex stats_mutex_;
    Stats stats_;

    // SPS/PPS storage for keyframe insertion
    std::mutex sps_pps_mutex_;
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;
};

} // namespace ss
