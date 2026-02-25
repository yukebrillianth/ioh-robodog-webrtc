#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>

namespace ss {

// Minimal HTTP file server for serving the web viewer
class HttpServer {
public:
    HttpServer(uint16_t port, const std::string& web_root);
    ~HttpServer();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void server_thread();
    void handle_client(int client_fd);
    std::string resolve_path(const std::string& uri) const;
    std::string get_mime_type(const std::string& path) const;
    void send_response(int fd, int status, const std::string& status_text,
                       const std::string& content_type, const std::string& body);
    void send_file(int fd, const std::string& path);

    uint16_t port_;
    std::string web_root_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;

    static const std::unordered_map<std::string, std::string> mime_types_;
};

} // namespace ss
