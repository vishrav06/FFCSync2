#include "ffcsync/registration_engine.hpp"

#include <stdexcept>

#include "ffcsync/rules.hpp"

namespace ffcsync {

RegistrationEngine::RegistrationEngine(const Catalog& catalog)
    : catalog_(catalog), state_(RegistrationState::empty_for(catalog)) {}

RegistrationResult RegistrationEngine::apply(const RegistrationRequest& req) {
    return apply_with_seq(last_seq_ + 1, req);
}

RegistrationResult RegistrationEngine::apply_with_seq(std::uint64_t seq,
                                                      const RegistrationRequest& req) {
    if (seq <= last_seq_) throw std::logic_error("sequence numbers must increase");
    last_seq_ = seq;
    return decide_and_commit(catalog_, state_, seq, req, check_static(catalog_, req));
}

std::vector<RegistrationResult> RegistrationEngine::run(
    const std::vector<RegistrationRequest>& requests) {
    std::vector<RegistrationResult> results;
    results.reserve(requests.size());
    for (const auto& req : requests) results.push_back(apply(req));
    return results;
}

}  // namespace ffcsync
