#include "ffcsync/server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "ffcsync/invariants.hpp"
#include "ffcsync/ipc_protocol.hpp"
#include "ffcsync/result_io.hpp"

namespace ffcsync {

struct Server::Connection {
    explicit Connection(int fd_, std::uint64_t id_) : fd(fd_), id(id_) {}
    ~Connection() { ::close(fd); }   // closed once the reader AND all pending responses are done

    // Several workers may answer requests of the same client at the same
    // time; the mutex keeps their frames from interleaving on the socket.
    bool send(const std::string& payload) {
        std::lock_guard lock(write_mu);
        return ipc::write_frame(fd, payload);
    }

    // Committer mode: appends every reply of a batch to `outbox`, then sends
    // the whole outbox with one write. Only the committer thread touches
    // outbox/queued; the write itself still takes write_mu, because a reader
    // thread may be answering a malformed message on the same socket.
    void flush() {
        {
            std::lock_guard lock(write_mu);
            ipc::write_all(fd, outbox);   // a disconnected client is ignored (EPIPE)
        }
        outbox.clear();
        queued = false;
    }

    const int fd;
    const std::uint64_t id;
    std::mutex write_mu;
    std::string outbox;
    bool queued = false;
};

namespace {

std::runtime_error sys_error(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

ipc::Response response_for(const RegistrationResult& r, std::uint64_t us) {
    return ipc::Response{r.request.request_id, r.seq, r.status, r.reason, us};
}

}  // namespace

Server::Server(const Catalog& catalog, ServerOptions options)
    : catalog_(catalog),
      options_(std::move(options)),
      engine_(catalog, options_.engine, [this] { flush_replies(); }) {
    if (::pipe2(stop_pipe_, O_CLOEXEC | O_NONBLOCK) < 0) throw sys_error("pipe2");
}

Server::~Server() {
    engine_.shutdown();
    for (auto& t : readers_) {
        if (t.joinable()) t.join();
    }
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (socket_bound_) ::unlink(options_.socket_path.c_str());
    ::close(stop_pipe_[0]);
    ::close(stop_pipe_[1]);
}

void Server::start() {
    const std::string& path = options_.socket_path;
    if (::access(path.c_str(), F_OK) == 0) {
        int probe = ipc::connect_unix(path);
        if (probe >= 0) {
            ::close(probe);
            throw std::runtime_error("another server is already listening on " + path);
        }
        ::unlink(path.c_str());   // stale file from a server that did not shut down cleanly
    }
    listen_fd_ = ipc::listen_unix(path);
    if (listen_fd_ < 0) throw sys_error("cannot listen on " + path);
    socket_bound_ = true;
}

void Server::request_stop() noexcept {
    const char b = 1;
    [[maybe_unused]] auto n = ::write(stop_pipe_[1], &b, 1);
}

void Server::run() {
    if (listen_fd_ < 0) start();

    while (true) {
        pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {stop_pipe_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            throw sys_error("poll");
        }
        if (fds[1].revents & POLLIN) break;
        if (!(fds[0].revents & POLLIN)) continue;

        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
            throw sys_error("accept");
        }
        auto conn = std::make_shared<Connection>(fd, next_conn_id_++);
        if (options_.engine.verbose) std::cerr << "[server] client " << conn->id << " connected\n";
        std::lock_guard lock(conns_mu_);
        conns_.push_back(conn);
        readers_.emplace_back(&Server::reader_loop, this, conn);
    }

    // 1. stop accepting new clients
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(options_.socket_path.c_str());
    socket_bound_ = false;

    // 2-3. wake every blocked reader (read() returns 0 after SHUT_RD) and join them
    {
        std::lock_guard lock(conns_mu_);
        for (auto& weak : conns_) {
            if (auto conn = weak.lock()) ::shutdown(conn->fd, SHUT_RD);
        }
    }
    for (auto& t : readers_) t.join();
    readers_.clear();

    // 4. drain queued requests and join the workers
    engine_.shutdown();

    // 5. outputs
    write_outputs();
}

void Server::reader_loop(std::shared_ptr<Connection> conn) {
    // Reads the socket in large chunks; every complete frame that arrived
    // together is decoded and admitted as one batch (consecutive seqs, in
    // stream order, one acquisition of the admission mutex).
    ipc::FrameReader in(conn->fd);
    const Completion on_done = reply_to(conn);
    std::vector<RegistrationRequest> batch;
    std::string payload;
    bool open = true;
    while (open) {
        batch.clear();
        do {
            ipc::ReadStatus st = in.next(payload);
            if (st == ipc::ReadStatus::Closed || st == ipc::ReadStatus::Error) {
                open = false;
                break;
            }
            if (st == ipc::ReadStatus::TooLarge) {
                // The rest of the stream cannot be framed reliably: answer and disconnect.
                conn->send(ipc::encode_response({"?", 0, Status::Rejected, Reason::Malformed, 0}));
                open = false;
                break;
            }
            auto req = ipc::decode_request(payload);
            if (!req) {
                // Rejected before admission: no sequence number, state untouched.
                if (options_.engine.verbose) {
                    std::cerr << "[server] client " << conn->id
                              << " sent a malformed message (no seq)\n";
                }
                conn->send(ipc::encode_response(
                    {ipc::guess_request_id(payload), 0, Status::Rejected, Reason::Malformed, 0}));
                continue;
            }
            batch.push_back(std::move(*req));
        } while (in.has_frame());
        // Requests read before a disconnect are still admitted.
        if (!batch.empty() && engine_.submit_batch(batch, on_done) == 0) break;   // engine shut down
    }
    if (options_.engine.verbose) std::cerr << "[server] client " << conn->id << " disconnected\n";
}

// The completion keeps the connection alive until the response is written,
// even if the client has already disconnected.
Completion Server::reply_to(std::shared_ptr<Connection> conn) {
    if (options_.engine.workers > 0 && options_.engine.mode == CommitMode::Committer) {
        // Runs on the committer thread: buffer, flushed by flush_replies().
        return [this, conn](const RegistrationResult& r, std::uint64_t us) {
            ipc::append_frame(conn->outbox, ipc::encode_response(response_for(r, us)));
            if (!conn->queued) {
                conn->queued = true;
                pending_replies_.push_back(conn);
            }
        };
    }
    return [conn](const RegistrationResult& r, std::uint64_t us) {
        conn->send(ipc::encode_response(response_for(r, us)));
    };
}

void Server::flush_replies() {
    for (auto& conn : pending_replies_) conn->flush();
    pending_replies_.clear();
}

std::vector<std::string> Server::invariant_violations() const {
    return check_invariants(catalog_, engine_.state());
}

void Server::write_outputs() const {
    const auto results = engine_.results();
    const auto violations = invariant_violations();
    auto summary = [&](std::ostream& o) {
        write_summary(o, results);
        o << "workers: " << options_.engine.workers << "\n"
          << "mode: " << to_string(options_.engine.mode) << "\n"
          << "queue_capacity: " << options_.engine.queue_capacity << "\n"
          << "queue_high_water: " << engine_.queue_high_water() << "\n";
        for (const auto& v : violations) o << "INVARIANT VIOLATION: " << v << "\n";
        o << "invariants: " << (violations.empty() ? "OK" : "FAILED") << "\n";
    };
    summary(std::cout);
    if (options_.out_dir.empty()) return;

    const auto& dir = options_.out_dir;
    write_file(dir / "admission.csv",
               [&](std::ostream& o) { write_admission_csv(o, engine_.admissions()); });
    write_file(dir / "results.csv", [&](std::ostream& o) { write_results_csv(o, results); });
    write_file(dir / "final_state.csv",
               [&](std::ostream& o) { write_state_csv(o, catalog_, engine_.state()); });
    write_file(dir / "summary.txt", summary);
}

}  // namespace ffcsync
