#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>
#include "config.hpp"

namespace ss {

inline void init_logger(const LoggingConfig& cfg) {
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink (always)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(console_sink);

    // File sink (optional)
    if (!cfg.file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            cfg.file,
            cfg.max_file_size_mb * 1024 * 1024,
            cfg.max_files
        );
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("stream-server", sinks.begin(), sinks.end());

    // Set level
    if (cfg.level == "trace") logger->set_level(spdlog::level::trace);
    else if (cfg.level == "debug") logger->set_level(spdlog::level::debug);
    else if (cfg.level == "info") logger->set_level(spdlog::level::info);
    else if (cfg.level == "warn") logger->set_level(spdlog::level::warn);
    else if (cfg.level == "error") logger->set_level(spdlog::level::err);
    else if (cfg.level == "critical") logger->set_level(spdlog::level::critical);
    else logger->set_level(spdlog::level::info);

    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

} // namespace ss
