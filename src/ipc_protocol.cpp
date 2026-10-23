#include "ffcsync/ipc_protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <vector>

namespace ffcsync::ipc {

namespace {

// Reads exactly n bytes. Returns n on success, 0 if EOF before any byte,
// -1 on error or EOF part-way through.
ssize_t read_full(int fd, char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return got == 0 ? 0 : -1;
        got += static_cast<std::size_t>(r);
    }
    return static_cast<ssize_t>(got);
}

bool write_full(int fd, const char* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        // MSG_NOSIGNAL: a disconnected peer yields EPIPE instead of killing us with SIGPIPE.
        ssize_t w = ::send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

std::vector<std::string_view> fields(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find('|', start);
        out.push_back(s.substr(start, pos == s.npos ? s.npos : pos - start));
        if (pos == s.npos) return out;
        start = pos + 1;
    }
}

// Identifiers end up in CSV files, so only allow a conservative character set.
bool valid_id(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

sockaddr_un make_addr(const std::string& path, bool& ok) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ok = path.size() < sizeof(addr.sun_path);
    if (ok) std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

}  // namespace

ReadStatus read_frame(int fd, std::string& payload) {
    unsigned char hdr[4];
    ssize_t r = read_full(fd, reinterpret_cast<char*>(hdr), sizeof hdr);
    if (r == 0) return ReadStatus::Closed;
    if (r < 0) return ReadStatus::Error;
    std::uint32_t len = (std::uint32_t{hdr[0]} << 24) | (std::uint32_t{hdr[1]} << 16) |
                        (std::uint32_t{hdr[2]} << 8) | std::uint32_t{hdr[3]};
    if (len > kMaxPayload) return ReadStatus::TooLarge;
    payload.resize(len);
    if (len > 0 && read_full(fd, payload.data(), len) != static_cast<ssize_t>(len)) {
        return ReadStatus::Error;
    }
    return ReadStatus::Ok;
}

bool write_frame(int fd, std::string_view payload) {
    if (payload.size() > kMaxPayload) return false;
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::string frame(4 + payload.size(), '\0');
    frame[0] = static_cast<char>((len >> 24) & 0xff);
    frame[1] = static_cast<char>((len >> 16) & 0xff);
    frame[2] = static_cast<char>((len >> 8) & 0xff);
    frame[3] = static_cast<char>(len & 0xff);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    // One send per frame keeps frames contiguous when several threads write
    // to the same socket under a per-connection mutex.
    return write_full(fd, frame.data(), frame.size());
}

std::string encode_request(const RegistrationRequest& req) {
    return "REQ|" + req.request_id + "|" + std::string(to_string(req.op)) + "|" + req.student_id +
           "|" + req.section_id;
}

std::optional<RegistrationRequest> decode_request(std::string_view payload) {
    auto f = fields(payload);
    if (f.size() != 5 || f[0] != "REQ") return std::nullopt;
    auto op = op_from_string(f[2]);
    if (!op || !valid_id(f[1]) || !valid_id(f[3]) || !valid_id(f[4])) return std::nullopt;
    return RegistrationRequest{std::string(f[1]), *op, std::string(f[3]), std::string(f[4])};
}

std::string guess_request_id(std::string_view payload) {
    auto f = fields(payload);
    if (f.size() >= 2 && f[0] == "REQ" && valid_id(f[1])) return std::string(f[1]);
    return "?";
}

std::string encode_response(const Response& r) {
    return "RES|" + r.request_id + "|" + std::to_string(r.seq) + "|" +
           std::string(to_string(r.status)) + "|" + std::string(to_string(r.reason)) + "|" +
           std::to_string(r.server_us);
}

std::optional<Response> decode_response(std::string_view payload) {
    auto f = fields(payload);
    if (f.size() != 6 || f[0] != "RES") return std::nullopt;
    auto seq = parse_u64(f[2]);
    auto status = status_from_string(f[3]);
    auto reason = reason_from_string(f[4]);
    auto us = parse_u64(f[5]);
    if (!seq || !status || !reason || !us) return std::nullopt;
    return Response{std::string(f[1]), *seq, *status, *reason, *us};
}

int listen_unix(const std::string& path, int backlog) {
    bool ok = false;
    sockaddr_un addr = make_addr(path, ok);
    if (!ok) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
        ::listen(fd, backlog) < 0) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int connect_unix(const std::string& path) {
    bool ok = false;
    sockaddr_un addr = make_addr(path, ok);
    if (!ok) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

}  // namespace ffcsync::ipc
