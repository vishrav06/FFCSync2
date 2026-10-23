// ffcsync_seq - the sequential reference engine (oracle).
//
//   ffcsync_seq run    --data DIR --requests FILE     --out DIR
//       Processes the request file in file order.
//   ffcsync_seq replay --data DIR --admission-log FILE --out DIR
//       Processes a server admission log in seq order, reusing the server's
//       sequence numbers, so its output can be diffed against the server's.
//
// Both write <out>/results.csv and <out>/final_state.csv, print a summary,
// run the invariant checker and exit 1 if any invariant is violated.
#include <chrono>
#include <iostream>

#include "cli.hpp"
#include "ffcsync/dataset.hpp"
#include "ffcsync/invariants.hpp"
#include "ffcsync/registration_engine.hpp"
#include "ffcsync/result_io.hpp"

using namespace ffcsync;

int main(int argc, char** argv) {
    if (argc < 2 || (std::string(argv[1]) != "run" && std::string(argv[1]) != "replay")) {
        std::cerr << "usage: ffcsync_seq run    --data DIR --requests FILE      --out DIR\n"
                     "       ffcsync_seq replay --data DIR --admission-log FILE --out DIR\n";
        return 2;
    }
    const std::string mode = argv[1];
    cli::Args args(argc, argv, 2);

    try {
        const Catalog catalog = load_catalog(args.get("data", "data"));
        const std::filesystem::path out = args.require("out");
        RegistrationEngine engine(catalog);
        std::vector<RegistrationResult> results;

        auto start = std::chrono::steady_clock::now();
        if (mode == "run") {
            results = engine.run(load_requests(args.require("requests")));
        } else {
            for (const auto& admitted : read_admission_csv(args.require("admission-log"))) {
                results.push_back(engine.apply_with_seq(admitted.seq, admitted.request));
            }
        }
        auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);

        write_file(out / "results.csv", [&](std::ostream& o) { write_results_csv(o, results); });
        write_file(out / "final_state.csv",
                   [&](std::ostream& o) { write_state_csv(o, catalog, engine.state()); });

        write_summary(std::cout, results);
        std::cout << "processing_ms: " << elapsed.count() << "\n";

        auto violations = check_invariants(catalog, engine.state());
        for (const auto& v : violations) std::cout << "INVARIANT VIOLATION: " << v << "\n";
        std::cout << "invariants: " << (violations.empty() ? "OK" : "FAILED") << "\n";
        return violations.empty() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
