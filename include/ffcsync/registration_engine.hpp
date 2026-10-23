// Sequential (single-threaded) registration engine.
//
// This is the reference implementation ("oracle"): applying a list of
// requests in order, one at a time, defines the correct outcome. The
// concurrent server must produce exactly the same results and final state as
// this engine replaying the server's admission order.
#pragma once

#include <vector>

#include "ffcsync/model.hpp"

namespace ffcsync {

class RegistrationEngine {
public:
    explicit RegistrationEngine(const Catalog& catalog);

    // Assigns the next sequence number and processes the request.
    RegistrationResult apply(const RegistrationRequest& req);

    // Processes a request with an externally assigned sequence number
    // (used when replaying a server admission log). `seq` must be greater
    // than every sequence number applied before.
    RegistrationResult apply_with_seq(std::uint64_t seq, const RegistrationRequest& req);

    std::vector<RegistrationResult> run(const std::vector<RegistrationRequest>& requests);

    const RegistrationState& state() const { return state_; }
    const Catalog& catalog() const { return catalog_; }

private:
    const Catalog& catalog_;
    RegistrationState state_;
    std::uint64_t last_seq_ = 0;
};

}  // namespace ffcsync
