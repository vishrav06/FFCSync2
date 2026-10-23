// The FFCS registration rules.
//
// The rules are split into two groups so the concurrent server can run the
// first group in parallel and only serialise the second:
//
//   check_static()  - depends only on the immutable Catalog (window, existence,
//                     prerequisites). Safe to call from any thread, no lock.
//   check_mutable() - depends on RegistrationState (seats, duplicates, clash,
//                     credits, buckets). Must be evaluated at commit time,
//                     because the state may change while a request waits.
//
// Rule order (first failure is reported):
//   WINDOW_CLOSED, UNKNOWN_STUDENT, UNKNOWN_SECTION,
//   ADD:  PREREQ_NOT_MET, ALREADY_REGISTERED, COURSE_ALREADY_TAKEN,
//         SECTION_FULL, TIMETABLE_CLASH, CREDIT_LIMIT, BUCKET_LIMIT
//   DROP: NOT_REGISTERED
#pragma once

#include "ffcsync/model.hpp"

namespace ffcsync {

Reason check_static(const Catalog& catalog, const RegistrationRequest& req);

// Precondition: check_static(catalog, req) == Reason::Ok.
Reason check_mutable(const Catalog& catalog, const RegistrationState& state,
                     const RegistrationRequest& req);

// Applies an accepted request to the state.
// Precondition: both checks returned Reason::Ok.
void apply_change(const Catalog& catalog, RegistrationState& state, const RegistrationRequest& req);

// The single commit step used by BOTH the sequential engine and the
// concurrent server: given the static verdict computed earlier, re-check the
// mutable rules against the current state and apply the change if allowed.
RegistrationResult decide_and_commit(const Catalog& catalog, RegistrationState& state,
                                     std::uint64_t seq, const RegistrationRequest& req,
                                     Reason static_verdict);

}  // namespace ffcsync
