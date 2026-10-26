// Runs a Server in a background thread of the test process and offers a
// tiny synchronous client for talking to it.
#pragma once

#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "ffcsync/ipc_protocol.hpp"
#include "ffcsync/server.hpp"

namespace ffcsync::test {

inline std::string unique_socket_path() {
    static std::atomic<int> counter{0};
    return "/tmp/ffcsync_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) +
           ".sock";
}

class TestServer {
public:
    TestServer(const Catalog& catalog, int workers, std::size_t queue_capacity = 16,
               CommitMode mode = CommitMode::Gate)
        : path_(unique_socket_path()),
          server_(catalog,
                  ServerOptions{path_, EngineOptions{workers, queue_capacity, false, mode}, {}}) {
        server_.start();
        thread_ = std::thread([this] { server_.run(); });
    }
    ~TestServer() { stop(); }

    void stop() {
        if (thread_.joinable()) {
            server_.request_stop();
            thread_.join();
        }
    }

    const std::string& path() const { return path_; }
    Server& server() { return server_; }

private:
    std::string path_;
    Server server_;
    std::thread thread_;
};

// Blocking request/response over one connection.
class TestClient {
public:
    explicit TestClient(const std::string& path) : fd_(ipc::connect_unix(path)) {}
    ~TestClient() { close(); }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    std::optional<ipc::Response> call_raw(const std::string& payload) {
        std::string reply;
        if (!ipc::write_frame(fd_, payload) || ipc::read_frame(fd_, reply) != ipc::ReadStatus::Ok) {
            return std::nullopt;
        }
        return ipc::decode_response(reply);
    }
    std::optional<ipc::Response> call(const RegistrationRequest& req) {
        return call_raw(ipc::encode_request(req));
    }

private:
    int fd_;
};

}  // namespace ffcsync::test
