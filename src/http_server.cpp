#include "http_server.hpp"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace ss {

const std::unordered_map<std::string, std::string> HttpServer::mime_types_ = {
    {".html", "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {".ttf",  "font/ttf"},
};

HttpServer::HttpServer(uint16_t port, const std::string& web_root)
    : port_(port)
    , web_root_(web_root)
{
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        spdlog::error("HTTP: Failed to create socket");
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::error("HTTP: Failed to bind to port {}", port_);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 16) < 0) {
        spdlog::error("HTTP: Failed to listen");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&HttpServer::server_thread, this);
    spdlog::info("HTTP server listening on http://0.0.0.0:{} (root: {})", port_, web_root_);
    return true;
}

void HttpServer::stop() {
    running_.store(false);
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HttpServer::server_thread() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running_.load()) {
                spdlog::debug("HTTP: Accept failed");
            }
            continue;
        }

        // Handle in a detached thread (fine for low-traffic file serving)
        std::thread([this, client_fd]() {
            handle_client(client_fd);
            close(client_fd);
        }).detach();
    }
}

void HttpServer::handle_client(int client_fd) {
    // Read request (small buffer is fine for HTTP headers)
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    // Parse first line: "GET /path HTTP/1.1"
    std::string request(buf);
    auto first_line_end = request.find("\r\n");
    if (first_line_end == std::string::npos) return;

    std::string first_line = request.substr(0, first_line_end);
    if (first_line.substr(0, 4) != "GET ") {
        send_response(client_fd, 405, "Method Not Allowed", "text/plain", "Only GET supported");
        return;
    }

    auto path_start = first_line.find(' ') + 1;
    auto path_end = first_line.find(' ', path_start);
    std::string uri = first_line.substr(path_start, path_end - path_start);

    // Strip query string
    auto query = uri.find('?');
    if (query != std::string::npos) {
        uri = uri.substr(0, query);
    }

    // Resolve file path
    std::string file_path = resolve_path(uri);
    if (file_path.empty()) {
        send_response(client_fd, 404, "Not Found", "text/html",
            "<html><body><h1>404 Not Found</h1></body></html>");
        return;
    }

    send_file(client_fd, file_path);
}

std::string HttpServer::resolve_path(const std::string& uri) const {
    std::string path = uri;

    // Default to index.html
    if (path == "/") {
        path = "/index.html";
    }

    // Build full path
    fs::path full = fs::path(web_root_) / path.substr(1); // remove leading /

    // Security: canonicalize and check it's inside web_root
    try {
        fs::path canonical = fs::canonical(full);
        fs::path root_canonical = fs::canonical(web_root_);
        auto root_str = root_canonical.string();

        if (canonical.string().substr(0, root_str.size()) != root_str) {
            spdlog::warn("HTTP: Path traversal attempt: {}", uri);
            return "";
        }

        if (fs::is_regular_file(canonical)) {
            return canonical.string();
        }
    } catch (...) {
        // File not found
    }

    return "";
}

std::string HttpServer::get_mime_type(const std::string& path) const {
    fs::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = mime_types_.find(ext);
    if (it != mime_types_.end()) {
        return it->second;
    }
    return "application/octet-stream";
}

void HttpServer::send_response(int fd, int status, const std::string& status_text,
                                const std::string& content_type, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string response = oss.str();
    send(fd, response.c_str(), response.size(), 0);
}

void HttpServer::send_file(int fd, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        send_response(fd, 500, "Internal Server Error", "text/plain", "Cannot read file");
        return;
    }

    // Read file
    std::string body((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

    std::string mime = get_mime_type(path);

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << mime << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n"
        << "\r\n";

    std::string header = oss.str();
    send(fd, header.c_str(), header.size(), 0);
    send(fd, body.c_str(), body.size(), 0);
}

} // namespace ss
