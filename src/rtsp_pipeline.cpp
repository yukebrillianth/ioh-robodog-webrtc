#include "rtsp_pipeline.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

namespace ss {

RtspPipeline::RtspPipeline(const AppConfig& config) : config_(config) {
    gst_init(nullptr, nullptr);
}

RtspPipeline::~RtspPipeline() {
    stop();
}

void RtspPipeline::set_nal_callback(NalUnitCallback cb) {
    nal_callback_ = std::move(cb);
}

bool RtspPipeline::start() {
    if (running_.load()) {
        spdlog::warn("Pipeline already running");
        return true;
    }

    stop_requested_.store(false);
    thread_ = std::thread(&RtspPipeline::pipeline_thread, this);
    return true;
}

void RtspPipeline::stop() {
    stop_requested_.store(true);
    running_.store(false);

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        appsink_ = nullptr;
        encoder_ = nullptr;
    }
}

void RtspPipeline::set_bitrate(int bitrate_kbps) {
    if (!encoder_ || !running_.load()) return;

    // Clamp to configured limits
    int clamped = std::max(config_.webrtc.video.min_bitrate_kbps,
                           std::min(bitrate_kbps, config_.webrtc.video.max_bitrate_kbps));

    if (is_hw_encode_) {
        // nvv4l2h264enc uses bits per second
        g_object_set(G_OBJECT(encoder_), "bitrate", (guint)(clamped * 1000), nullptr);
    } else {
        // x264enc uses kbps
        g_object_set(G_OBJECT(encoder_), "bitrate", (guint)clamped, nullptr);
    }

    spdlog::info("Encoder bitrate adjusted to {} kbps", clamped);
}

RtspPipeline::Stats RtspPipeline::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void RtspPipeline::build_pipeline() {
    std::string pipeline_desc;

    bool use_test_source = false;
#ifdef ENABLE_TEST_MODE
    use_test_source = config_.rtsp.url.empty();
#endif

    if (use_test_source) {
        // Test pattern source for development/verification
        spdlog::info("Using test pattern source (no RTSP URL configured)");
        pipeline_desc =
            "videotestsrc is-live=true pattern=ball ! "
            "video/x-raw,width=1280,height=720,framerate=30/1 ! ";

#ifdef JETSON_PLATFORM
        if (config_.encoding.hw_encode) {
            pipeline_desc +=
                "nvvidconv ! "
                "video/x-raw(memory:NVMM),format=NV12 ! "
                "nvv4l2h264enc "
                "bitrate=" + std::to_string(config_.webrtc.video.bitrate_kbps * 1000) + " "
                "maxperf-enable=1 "
                "preset-level=1 "
                "insert-sps-pps=1 "
                "idrinterval=" + std::to_string(config_.encoding.idr_interval) + " ! ";
        } else {
#endif
            pipeline_desc +=
                "x264enc tune=zerolatency speed-preset=ultrafast "
                "bitrate=" + std::to_string(config_.webrtc.video.bitrate_kbps) + " "
                "key-int-max=" + std::to_string(config_.encoding.idr_interval) + " "
                "bframes=0 ! ";
#ifdef JETSON_PLATFORM
        }
#endif
        pipeline_desc +=
            "video/x-h264,profile=baseline ! "
            "h264parse config-interval=1 ! "
            "appsink name=sink emit-signals=true sync=false max-buffers=5 drop=true";

    } else if (config_.encoding.passthrough) {
        // Passthrough mode: relay H.264 from RTSP directly
        spdlog::info("Using RTSP passthrough mode (no re-encode)");
        pipeline_desc =
            "rtspsrc location=" + config_.rtsp.url + " "
            "latency=" + std::to_string(config_.rtsp.latency_ms) + " "
            "protocols=" + config_.rtsp.transport + " "
            "is-live=true "
            "buffer-mode=auto "
            "do-retransmission=false "
            "drop-on-latency=true ! "
            "rtph264depay ! "
            "h264parse config-interval=1 ! "
            "video/x-h264,stream-format=byte-stream,alignment=au ! "
            "appsink name=sink emit-signals=true sync=false max-buffers=5 drop=true";

    } else {
        // Re-encode mode: decode + encode with bitrate control
        spdlog::info("Using re-encode mode");
        pipeline_desc =
            "rtspsrc location=" + config_.rtsp.url + " "
            "latency=" + std::to_string(config_.rtsp.latency_ms) + " "
            "protocols=" + config_.rtsp.transport + " "
            "is-live=true "
            "buffer-mode=auto "
            "do-retransmission=false "
            "drop-on-latency=true ! "
            "rtph264depay ! "
            "h264parse config-interval=-1 ! "
            "video/x-h264,stream-format=byte-stream,alignment=au ! ";

#ifdef JETSON_PLATFORM
        // Jetson: always use HW decoder, optionally HW encoder
        if (config_.encoding.hw_encode) {
            is_hw_encode_ = true;
            // HW decode → HW encode
            pipeline_desc +=
                "nvv4l2decoder enable-max-performance=1 ! "
                "nvv4l2h264enc name=enc "
                "bitrate=" + std::to_string(config_.webrtc.video.bitrate_kbps * 1000) + " "
                "peak-bitrate=" + std::to_string(config_.webrtc.video.max_bitrate_kbps * 1000) + " "
                "maxperf-enable=1 "
                "preset-level=1 "
                "control-rate=1 "
                "insert-sps-pps=1 "
                "idrinterval=" + std::to_string(config_.encoding.idr_interval) + " ! ";
        } else {
            // HW decode → SW encode
            is_hw_encode_ = false;
            pipeline_desc +=
                "nvv4l2decoder enable-max-performance=1 ! "
                "nvvidconv ! video/x-raw,format=I420 ! "
                "x264enc name=enc tune=zerolatency speed-preset=ultrafast "
                "bitrate=" + std::to_string(config_.webrtc.video.bitrate_kbps) + " "
                "vbv-buf-capacity=" + std::to_string(config_.webrtc.video.max_bitrate_kbps) + " "
                "key-int-max=" + std::to_string(config_.encoding.idr_interval) + " "
                "bframes=0 ! ";
        }
#else
        // Non-Jetson: software decode + encode
        is_hw_encode_ = false;
        pipeline_desc +=
            "avdec_h264 ! videoconvert ! "
            "x264enc name=enc tune=zerolatency speed-preset=ultrafast "
            "bitrate=" + std::to_string(config_.webrtc.video.bitrate_kbps) + " "
            "vbv-buf-capacity=" + std::to_string(config_.webrtc.video.max_bitrate_kbps) + " "
            "key-int-max=" + std::to_string(config_.encoding.idr_interval) + " "
            "bframes=0 ! ";
#endif
        pipeline_desc +=
            "video/x-h264,stream-format=byte-stream,alignment=au ! "
            "h264parse config-interval=1 ! "
            "appsink name=sink emit-signals=true sync=false max-buffers=5 drop=true";
    }

    spdlog::info("Pipeline: {}", pipeline_desc);

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (error) {
        std::string err_msg = error->message;
        g_error_free(error);
        throw std::runtime_error("Failed to create pipeline: " + err_msg);
    }

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) {
        throw std::runtime_error("Failed to find appsink element");
    }

    // Grab encoder element for dynamic bitrate control
    encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
    if (encoder_) {
        spdlog::info("Encoder found — dynamic bitrate control enabled");
    }

    // Configure appsink callbacks
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &RtspPipeline::on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this, nullptr);

    gst_object_unref(appsink_);
}

GstFlowReturn RtspPipeline::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<RtspPipeline*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        // Get timestamp in microseconds
        uint64_t timestamp_us = 0;
        if (GST_BUFFER_PTS_IS_VALID(buffer)) {
            timestamp_us = GST_BUFFER_PTS(buffer) / 1000; // ns → µs
        } else {
            auto now = std::chrono::steady_clock::now();
            timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
        }

        // Deliver NAL units to callback
        if (self->nal_callback_ && map.size > 0) {
            self->nal_callback_(map.data, map.size, timestamp_us);
        }

        // Update stats
        {
            std::lock_guard<std::mutex> lock(self->stats_mutex_);
            self->stats_.frames_received++;
            self->stats_.bytes_received += map.size;
            self->stats_.connected = true;
        }

        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void RtspPipeline::pipeline_thread() {
    spdlog::info("Pipeline thread started");

    while (!stop_requested_.load()) {
        try {
            build_pipeline();
        } catch (const std::exception& e) {
            spdlog::error("Failed to build pipeline: {}", e.what());
            attempt_reconnect();
            continue;
        }

        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            spdlog::error("Failed to set pipeline to PLAYING");
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            appsink_ = nullptr;
            attempt_reconnect();
            continue;
        }

        running_.store(true);
        spdlog::info("Pipeline is PLAYING");

        // Run the message loop
        GstBus* bus = gst_element_get_bus(pipeline_);
        bool pipeline_ok = true;

        while (!stop_requested_.load() && pipeline_ok) {
            GstMessage* msg = gst_bus_timed_pop(bus, 500 * GST_MSECOND);
            if (msg) {
                handle_bus_message(msg);

                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ||
                    GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    pipeline_ok = false;
                }
                gst_message_unref(msg);
            }
        }

        gst_object_unref(bus);
        running_.store(false);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.connected = false;
        }

        // Cleanup pipeline
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        appsink_ = nullptr;

        if (!stop_requested_.load()) {
            spdlog::warn("Pipeline ended unexpectedly, will reconnect...");
            attempt_reconnect();
        }
    }

    spdlog::info("Pipeline thread stopped");
}

void RtspPipeline::handle_bus_message(GstMessage* msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug_info = nullptr;
            gst_message_parse_error(msg, &err, &debug_info);
            spdlog::error("GStreamer error: {} ({})",
                         err->message, debug_info ? debug_info : "no debug info");
            g_error_free(err);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* debug_info = nullptr;
            gst_message_parse_warning(msg, &err, &debug_info);
            spdlog::warn("GStreamer warning: {} ({})",
                        err->message, debug_info ? debug_info : "no debug info");
            g_error_free(err);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_EOS:
            spdlog::warn("End of stream received");
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                spdlog::debug("Pipeline state: {} -> {}",
                            gst_element_state_get_name(old_state),
                            gst_element_state_get_name(new_state));
            }
            break;
        }
        default:
            break;
    }
}

void RtspPipeline::attempt_reconnect() {
    if (stop_requested_.load()) return;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.reconnect_count++;
    }

    int interval_ms = config_.rtsp.reconnect_interval_ms;
    spdlog::info("Reconnecting in {}ms...", interval_ms);

    // Sleep in small increments to allow quick shutdown
    int elapsed = 0;
    while (elapsed < interval_ms && !stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        elapsed += 100;
    }
}

} // namespace ss
