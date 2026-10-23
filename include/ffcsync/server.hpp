// Registration server: owns the registration state and serves clients over
// a Unix domain socket.
//
// Threads inside the server process:
//   * main thread       - accept loop (poll on listening socket + self-pipe)
//   * reader thread x C - one per client connection: reads frames, decodes,
//                         calls ConcurrentEngine::submit() (the admission point)
//   * worker thread x N - inside ConcurrentEngine; process and commit requests,
//                         then write the response back on the client's socket
//
// Graceful shutdown (request_stop(), e.g. from SIGINT/SIGTERM):
//   1. stop accepting: close and unlink the listening socket
//   2. shutdown(SHUT_RD) every client socket so blocked readers return
//   3. join reader threads (no more admissions)
//   4. ConcurrentEngine::shutdown(): workers drain the queue and are joined
//   5. write output files (admission log, results, final state, summary)
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ffcsync/concurrent_engine.hpp"
#include "ffcsync/model.hpp"

namespace ffcsync {

struct ServerOptions {
    std::string socket_path = "/tmp/ffcsync.sock";
    EngineOptions engine;
    std::filesystem::path out_dir;   // empty = do not write output files
};

class Server {
public:
    Server(const Catalog& catalog, ServerOptions options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and listens. Removes a stale socket file left by a crashed server,
    // but throws if another live server is already listening on the path.
    void start();

    // Accept loop. Returns after request_stop() once shutdown is complete.
    void run();

    // Async-signal-safe: only writes one byte to the self-pipe.
    void request_stop() noexcept;

    // Valid after run() returns.
    const ConcurrentEngine& engine() const { return engine_; }
    std::vector<std::string> invariant_violations() const;

private:
    struct Connection;

    void reader_loop(std::shared_ptr<Connection> conn);
    void write_outputs() const;

    const Catalog& catalog_;
    const ServerOptions options_;
    ConcurrentEngine engine_;

    int listen_fd_ = -1;
    int stop_pipe_[2] = {-1, -1};
    bool socket_bound_ = false;

    std::mutex conns_mu_;
    std::vector<std::weak_ptr<Connection>> conns_;
    std::vector<std::thread> readers_;
    std::atomic<std::uint64_t> next_conn_id_{1};
};

}  // namespace ffcsync
