// ffcsync_client - registration client / workload generator process.
//
// Single request:
//   ffcsync_client [--socket PATH] --op ADD --student S001 --section CSE2005-A1 [--id ID]
// Raw payload (to demonstrate MALFORMED handling):
//   ffcsync_client [--socket PATH] --raw "REQ|x|BOGUS|S001|CSE2005-A1"
// Workload file (pipelined: a sender thread keeps up to --max-inflight
// requests outstanding while the main thread receives responses):
//   ffcsync_client [--socket PATH] --requests FILE [--out FILE] [--max-inflight K]
//                  [--metrics FILE --label NAME] [--quiet]
//
// The client never touches registration state; it only sends requests and
// records the server's answers.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <semaphore>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "cli.hpp"
#include "ffcsync/dataset.hpp"
#include "ffcsync/ipc_protocol.hpp"

using namespace ffcsync;
using Clock = std::chrono::steady_clock;

namespace {

void print_response(const ipc::Response& r) {
    std::cout << r.request_id << " seq=" << r.seq << " " << to_string(r.status) << " "
              << to_string(r.reason) << "\n";
}

int connect_or_die(const std::string& path) {
    int fd = ipc::connect_unix(path);
    if (fd < 0) {
        std::perror(("connect " + path).c_str());
        std::exit(2);
    }
    return fd;
}

int send_one(int fd, const std::string& payload) {
    std::string reply;
    if (!ipc::write_frame(fd, payload) || ipc::read_frame(fd, reply) != ipc::ReadStatus::Ok) {
        std::cerr << "error: server closed the connection\n";
        return 1;
    }
    auto resp = ipc::decode_response(reply);
    if (!resp) {
        std::cerr << "error: malformed response '" << reply << "'\n";
        return 1;
    }
    print_response(*resp);
    return 0;
}

struct Sample {
    RegistrationRequest req;
    Clock::time_point sent;
    std::optional<ipc::Response> resp;
    std::uint64_t latency_us = 0;
};

int run_workload(int fd, const cli::Args& args) {
    auto requests = load_requests(args.require("requests"));
    const long max_inflight = std::max(1L, args.get_int("max-inflight", 64));
    const bool quiet = args.has("quiet");

    std::vector<Sample> samples(requests.size());
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (!index.emplace(requests[i].request_id, i).second) {
            cli::Args::fail("duplicate request_id " + requests[i].request_id);
        }
        samples[i].req = requests[i];
    }

    // Counting semaphore = number of request "slots" the sender may still use.
    std::counting_semaphore<> window(max_inflight);
    std::mutex sent_mu;   // guards Sample::sent (written by sender, read by receiver)
    const auto start = Clock::now();

    std::thread sender([&] {
        for (auto& s : samples) {
            window.acquire();
            {
                std::lock_guard lock(sent_mu);
                s.sent = Clock::now();
            }
            if (!ipc::write_frame(fd, ipc::encode_request(s.req))) break;
        }
    });

    std::size_t received = 0;
    std::string payload;
    while (received < samples.size()) {
        if (ipc::read_frame(fd, payload) != ipc::ReadStatus::Ok) break;
        const auto now = Clock::now();
        auto resp = ipc::decode_response(payload);
        auto it = resp ? index.find(resp->request_id) : index.end();
        if (it == index.end()) {
            std::cerr << "warning: unexpected response '" << payload << "'\n";
            continue;
        }
        Sample& s = samples[it->second];
        {
            std::lock_guard lock(sent_mu);
            s.latency_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - s.sent).count());
        }
        s.resp = *resp;
        ++received;
        window.release();
        if (!quiet) print_response(*resp);
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (received < samples.size()) {
        ::shutdown(fd, SHUT_RDWR);   // unblock the sender if it is stuck
        window.release(max_inflight);
    }
    sender.join();

    // ---- outputs ----
    std::size_t accepted = 0;
    std::vector<std::uint64_t> lat;
    std::uint64_t server_us_sum = 0;
    for (const auto& s : samples) {
        if (!s.resp) continue;
        if (s.resp->status == Status::Accepted) ++accepted;
        lat.push_back(s.latency_us);
        server_us_sum += s.resp->server_us;
    }
    std::sort(lat.begin(), lat.end());
    double avg = 0;
    for (auto v : lat) avg += static_cast<double>(v);
    avg = lat.empty() ? 0 : avg / static_cast<double>(lat.size());
    const std::uint64_t p95 = lat.empty() ? 0 : lat[(lat.size() * 95 + 99) / 100 - 1];
    const double throughput = total_ms > 0 ? received / (total_ms / 1000.0) : 0;
    const double avg_server = lat.empty() ? 0 : static_cast<double>(server_us_sum) / lat.size();

    if (args.has("out")) {
        std::ofstream out(args.get("out", ""));
        out << "request_id,op,student_id,section_id,seq,status,reason,latency_us,server_us\n";
        for (const auto& s : samples) {
            if (!s.resp) continue;
            out << s.req.request_id << ',' << to_string(s.req.op) << ',' << s.req.student_id << ','
                << s.req.section_id << ',' << s.resp->seq << ',' << to_string(s.resp->status)
                << ',' << to_string(s.resp->reason) << ',' << s.latency_us << ','
                << s.resp->server_us << '\n';
        }
    }
    if (args.has("metrics")) {
        const std::string file = args.get("metrics", "");
        const bool fresh = !std::ifstream(file).good();
        std::ofstream m(file, std::ios::app);
        if (fresh) {
            m << "label,requests,received,accepted,rejected,total_ms,throughput_rps,"
                 "avg_latency_us,p95_latency_us,avg_server_us\n";
        }
        m << args.get("label", "run") << ',' << samples.size() << ',' << received << ','
          << accepted << ',' << received - accepted << ',' << total_ms << ',' << throughput << ','
          << avg << ',' << p95 << ',' << avg_server << '\n';
    }

    std::cerr << "sent=" << samples.size() << " received=" << received << " accepted=" << accepted
              << " rejected=" << received - accepted << " total_ms=" << total_ms
              << " throughput_rps=" << throughput << " avg_latency_us=" << avg
              << " p95_latency_us=" << p95 << "\n";
    return received == samples.size() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    cli::Args args(argc, argv, 1, {"quiet"});
    try {
        const std::string path = args.get("socket", "/tmp/ffcsync.sock");
        int fd = connect_or_die(path);
        int rc;
        if (args.has("raw")) {
            rc = send_one(fd, args.get("raw", ""));
        } else if (args.has("requests")) {
            rc = run_workload(fd, args);
        } else {
            auto op = op_from_string(args.require("op"));
            if (!op) cli::Args::fail("--op must be ADD or DROP");
            RegistrationRequest req{args.get("id", "cli-" + std::to_string(::getpid())), *op,
                                    args.require("student"), args.require("section")};
            rc = send_one(fd, ipc::encode_request(req));
        }
        ::close(fd);
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
