// Client <-> server protocol over a Unix domain stream socket.
// Wire format: see README.md ("Wire format")
//
// Framing: every message is a 4-byte big-endian payload length followed by
// that many bytes of ASCII payload (max kMaxPayload). A stream socket may
// split or merge writes arbitrarily, so read_frame/write_frame loop until
// the whole frame has been transferred.
//
// Payloads ('|'-separated fields):
//   request : REQ|<request_id>|<ADD|DROP>|<student_id>|<section_id>
//   response: RES|<request_id>|<seq>|<ACCEPTED|REJECTED>|<reason>|<server_us>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ffcsync/model.hpp"

namespace ffcsync::ipc {

constexpr std::size_t kMaxPayload = 1024;

enum class ReadStatus {
    Ok,         // a complete frame was read
    Closed,     // peer closed the connection cleanly between frames
    TooLarge,   // length prefix exceeds kMaxPayload (stream cannot be resynchronised)
    Error,      // I/O error or connection closed in the middle of a frame
};

ReadStatus read_frame(int fd, std::string& payload);
bool write_frame(int fd, std::string_view payload);   // false on error / disconnected peer

// Appends one frame (length prefix + payload) to `out`, so several frames can
// be sent with a single write_all(). Precondition: payload.size() <= kMaxPayload.
void append_frame(std::string& out, std::string_view payload);
bool write_all(int fd, std::string_view bytes);       // false on error / disconnected peer

// Buffered frame reader for one stream socket. read_frame() costs two read()
// calls per frame; FrameReader reads up to `chunk` bytes at a time and hands
// out every complete frame already buffered without another system call.
// Same ReadStatus meanings as read_frame().
class FrameReader {
public:
    explicit FrameReader(int fd, std::size_t chunk = 64 * 1024) : fd_(fd), chunk_(chunk) {}

    // Blocks (reads the socket) only if no complete frame is buffered.
    ReadStatus next(std::string& payload);
    // True if next() can return without reading the socket.
    bool has_frame() const;

private:
    int fd_;
    std::size_t chunk_;
    std::string buf_;
    std::size_t pos_ = 0;   // start of the first unconsumed byte in buf_
};

struct Response {
    std::string request_id;
    std::uint64_t seq = 0;
    Status status = Status::Rejected;
    Reason reason = Reason::Malformed;
    std::uint64_t server_us = 0;   // time spent inside the server worker
};

std::string encode_request(const RegistrationRequest& req);
// Returns nullopt for anything that is not a well-formed request.
std::optional<RegistrationRequest> decode_request(std::string_view payload);
// Best-effort extraction of the request id from a malformed payload, so the
// MALFORMED response can still be matched by the client ("?" if none).
std::string guess_request_id(std::string_view payload);

std::string encode_response(const Response& resp);
std::optional<Response> decode_response(std::string_view payload);

// Socket helpers. Both return -1 and set errno on failure.
int listen_unix(const std::string& path, int backlog = 64);
int connect_unix(const std::string& path);

}  // namespace ffcsync::ipc
