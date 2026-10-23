// ffcsync_server - the registration server process.
//
//   ffcsync_server [--data DIR] [--socket PATH] [--workers N] [--queue-capacity Q]
//                  [--out DIR] [--verbose]
//
//   --workers 0 runs the sequential server (requests processed inline).
// Stops gracefully on SIGINT / SIGTERM; exits 1 if an invariant is violated.
#include <csignal>
#include <iostream>

#include "cli.hpp"
#include "ffcsync/dataset.hpp"
#include "ffcsync/server.hpp"

using namespace ffcsync;

namespace {

Server* g_server = nullptr;

extern "C" void on_signal(int) {
    if (g_server) g_server->request_stop();   // async-signal-safe (one write())
}

}  // namespace

int main(int argc, char** argv) {
    cli::Args args(argc, argv, 1, {"verbose"});
    try {
        const Catalog catalog = load_catalog(args.get("data", "data"));

        ServerOptions opts;
        opts.socket_path = args.get("socket", "/tmp/ffcsync.sock");
        opts.engine.workers = static_cast<int>(args.get_int("workers", 4));
        opts.engine.queue_capacity = static_cast<std::size_t>(args.get_int("queue-capacity", 64));
        opts.engine.verbose = args.has("verbose");
        opts.out_dir = args.get("out", "");
        if (opts.engine.workers < 0) cli::Args::fail("--workers must be >= 0");
        if (opts.engine.queue_capacity == 0) cli::Args::fail("--queue-capacity must be > 0");

        Server server(catalog, opts);
        server.start();

        g_server = &server;
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        std::signal(SIGPIPE, SIG_IGN);

        std::cout << "ffcsync_server listening on " << opts.socket_path << " (workers="
                  << opts.engine.workers << ", queue=" << opts.engine.queue_capacity << ")"
                  << std::endl;
        server.run();
        g_server = nullptr;
        std::cout << "ffcsync_server stopped" << std::endl;
        return server.invariant_violations().empty() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
