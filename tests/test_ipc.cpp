// IPC tests: framing, message codec, and the server over a real Unix socket.
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <thread>

#include "doctest.h"
#include "fixtures.hpp"
#include "server_harness.hpp"
#include "ffcsync/ipc_protocol.hpp"
#include "ffcsync/registration_engine.hpp"

using namespace ffcsync;
using namespace ffcsync::test;

namespace {

struct SocketPair {
    int a = -1, b = -1;
    SocketPair() {
        int fds[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        a = fds[0];
        b = fds[1];
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

std::string frame_bytes(const std::string& payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string out{static_cast<char>(n >> 24), static_cast<char>(n >> 16),
                    static_cast<char>(n >> 8), static_cast<char>(n)};
    return out + payload;
}

}  // namespace

TEST_SUITE("ipc") {

TEST_CASE("frames survive being split into single-byte writes") {
    SocketPair sp;
    const std::string bytes = frame_bytes("hello") + frame_bytes("") + frame_bytes("world!");
    std::thread writer([&] {
        for (char c : bytes) {
            REQUIRE(::write(sp.a, &c, 1) == 1);
        }
        ::close(sp.a);
        sp.a = -1;
    });
    std::string p;
    CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::Ok);
    CHECK(p == "hello");
    CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::Ok);
    CHECK(p.empty());
    CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::Ok);
    CHECK(p == "world!");
    CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::Closed);
    writer.join();
}

TEST_CASE("oversized frames and truncated frames are reported") {
    {
        SocketPair sp;
        std::string hdr{0, 0, 0x10, 0};   // 4096 bytes > kMaxPayload
        REQUIRE(::write(sp.a, hdr.data(), 4) == 4);
        std::string p;
        CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::TooLarge);
        CHECK_FALSE(ipc::write_frame(sp.a, std::string(ipc::kMaxPayload + 1, 'x')));
    }
    {
        SocketPair sp;
        std::string partial = frame_bytes("abcdef").substr(0, 7);
        REQUIRE(::write(sp.a, partial.data(), partial.size()) == 7);
        ::close(sp.a);
        sp.a = -1;
        std::string p;
        CHECK(ipc::read_frame(sp.b, p) == ipc::ReadStatus::Error);
    }
}

TEST_CASE("request and response codec") {
    RegistrationRequest req{"r-1", Op::Drop, "S001", "CSE2005-A1"};
    auto back = ipc::decode_request(ipc::encode_request(req));
    REQUIRE(back);
    CHECK(back->request_id == "r-1");
    CHECK(back->op == Op::Drop);
    CHECK(back->section_id == "CSE2005-A1");

    ipc::Response resp{"r-1", 42, Status::Accepted, Reason::Ok, 17};
    auto rb = ipc::decode_response(ipc::encode_response(resp));
    REQUIRE(rb);
    CHECK(rb->seq == 42);
    CHECK(rb->status == Status::Accepted);
    CHECK(rb->server_us == 17);

    for (const char* bad : {"", "REQ", "REQ|a|ADD|S001", "REQ|a|ADD|S001|X|extra", "XYZ|a|ADD|S|X",
                            "REQ|a|JOIN|S001|X", "REQ||ADD|S001|X", "REQ|a,b|ADD|S001|X"}) {
        CAPTURE(bad);
        CHECK_FALSE(ipc::decode_request(bad));
    }
    CHECK(ipc::guess_request_id("REQ|abc|JOIN|S|X") == "abc");
    CHECK(ipc::guess_request_id("garbage") == "?");
}

TEST_CASE("server answers every request with the oracle's result") {
    for (int workers : {0, 2}) {
        CAPTURE(workers);
        auto requests = load_requests(data_dir() / "workloads" / "basic.csv");
        RegistrationEngine oracle(catalog());
        auto expected = oracle.run(requests);

        TestServer ts(catalog(), workers);
        TestClient c(ts.path());
        REQUIRE(c.ok());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto resp = c.call(requests[i]);
            REQUIRE(resp);
            CHECK(resp->request_id == requests[i].request_id);
            CHECK(resp->seq == expected[i].seq);
            CHECK(resp->status == expected[i].status);
            CHECK(resp->reason == expected[i].reason);
        }
        ts.stop();
        CHECK(ts.server().engine().state() == oracle.state());
        CHECK(ts.server().invariant_violations().empty());
    }
}

TEST_CASE("malformed messages are rejected without a sequence number") {
    TestServer ts(catalog(), 0);
    TestClient c(ts.path());
    auto r = c.call_raw("REQ|bad-1|JOIN|S001|CSE2005-A1");
    REQUIRE(r);
    CHECK(r->request_id == "bad-1");
    CHECK(r->seq == 0);
    CHECK(r->reason == Reason::Malformed);
    // The connection is still usable and the next valid request gets seq 1.
    auto ok = c.call(add("S001", "CSE2005-A1", "good-1"));
    REQUIRE(ok);
    CHECK(ok->seq == 1);
    CHECK(ok->status == Status::Accepted);
}

TEST_CASE("oversized frame gets MALFORMED and the connection is closed") {
    TestServer ts(catalog(), 0);
    TestClient c(ts.path());
    std::string hdr{0, 0, 0x10, 0};
    REQUIRE(::write(c.fd(), hdr.data(), 4) == 4);
    std::string reply;
    REQUIRE(ipc::read_frame(c.fd(), reply) == ipc::ReadStatus::Ok);
    CHECK(ipc::decode_response(reply)->reason == Reason::Malformed);
    CHECK(ipc::read_frame(c.fd(), reply) == ipc::ReadStatus::Closed);
    TestClient c2(ts.path());   // server still serves others
    CHECK(c2.call(add("S001", "CSE2005-A1"))->status == Status::Accepted);
}

TEST_CASE("client disconnect does not break the server") {
    for (int workers : {0, 2}) {
        CAPTURE(workers);
        TestServer ts(catalog(), workers);
        {
            TestClient c(ts.path());
            // Send without reading the response, then vanish.
            REQUIRE(ipc::write_frame(c.fd(), ipc::encode_request(add("S001", "CSE2005-B1", "x"))));
            std::string half = frame_bytes("REQ|y|ADD|S002|CSE2005-B1").substr(0, 6);
            REQUIRE(::write(c.fd(), half.data(), half.size()) == 6);
        }
        // Wait until the vanished client's request has been admitted.
        for (int i = 0; i < 2000 && ts.server().engine().admissions().empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(ts.server().engine().admissions().size() == 1);
        TestClient c2(ts.path());
        auto r = c2.call(add("S002", "CSE2005-B1", "z"));
        REQUIRE(r);
        // The disconnected client's admitted request still committed first.
        CHECK(r->reason == Reason::SectionFull);
        CHECK(r->seq == 2);
    }
}

TEST_CASE("socket file lifecycle") {
    std::string path;
    {
        TestServer ts(catalog(), 1);
        path = ts.path();
        CHECK(std::filesystem::exists(path));
        // A second server on the same path must refuse to start.
        Server other(catalog(), ServerOptions{path, EngineOptions{0, 4, false}, {}});
        CHECK_THROWS(other.start());
        ts.stop();
        CHECK_FALSE(std::filesystem::exists(path));
    }
    // A stale socket file (no listener) is replaced.
    int fd = ipc::listen_unix(path);
    REQUIRE(fd >= 0);
    ::close(fd);   // file remains, nobody listening
    REQUIRE(std::filesystem::exists(path));
    Server s(catalog(), ServerOptions{path, EngineOptions{0, 4, false}, {}});
    CHECK_NOTHROW(s.start());
}

}  // TEST_SUITE
