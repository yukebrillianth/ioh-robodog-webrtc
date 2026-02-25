cmake_minimum_required(VERSION 3.16)
# Compatibility for dependencies with old cmake_minimum_required
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(stream-server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# ─── Options ───────────────────────────────────────────────────────────────────
option(ENABLE_JETSON "Enable NVIDIA Jetson hardware acceleration" OFF)
option(ENABLE_TEST_MODE "Build with test pattern source support" ON)

# ─── Dependencies via FetchContent ─────────────────────────────────────────────
include(FetchContent)

# libdatachannel — WebRTC + WebSocket
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.22.5
    GIT_SHALLOW    TRUE
)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libdatachannel)

# spdlog — Logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# nlohmann_json — JSON
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# yaml-cpp — Configuration
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# ─── GStreamer (system dependency) ─────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-rtp-1.0
    gstreamer-video-1.0
)

# ─── Find Threads ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/rtsp_pipeline.cpp
    src/webrtc_server.cpp
    src/signaling_server.cpp
    src/peer_connection.cpp
    src/http_server.cpp
)

# ─── Executable ────────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GST_INCLUDE_DIRS}
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${GST_LIBRARY_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    LibDataChannel::LibDataChannel
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    ${GST_LIBRARIES}
    Threads::Threads
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

if(ENABLE_JETSON)
    target_compile_definitions(${PROJECT_NAME} PRIVATE JETSON_PLATFORM)
endif()

if(ENABLE_TEST_MODE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_TEST_MODE)
endif()

# ─── Install ───────────────────────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES config.yaml DESTINATION etc/stream-server)
install(FILES systemd/stream-server.service DESTINATION lib/systemd/system)
install(DIRECTORY web/ DESTINATION share/stream-server/web)