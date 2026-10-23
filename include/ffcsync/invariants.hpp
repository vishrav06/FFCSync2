// Final-state invariant checker.
//
// Recomputes everything derivable from the per-student section sets and
// compares it with what the engine stored. Any mismatch indicates a bug
// (or, in the concurrent server, a race). Checked invariants:
//   1. every section: 0 <= enrolled <= capacity
//   2. every section: enrolled == number of students registered in it
//   3. every registration refers to an existing section; at most one section per course
//   4. stored credits == sum of course credits; credits <= semester maximum
//   5. stored bucket credits == recomputed; each bucket within its limit
//   6. no two registered sections of a student share a timetable slot,
//      and the stored slot set equals the union of the sections' slots
//   7. prerequisites of every registered course are satisfied
#pragma once

#include <string>
#include <vector>

#include "ffcsync/model.hpp"

namespace ffcsync {

// Returns a human-readable description of each violation; empty means OK.
std::vector<std::string> check_invariants(const Catalog& catalog, const RegistrationState& state);

}  // namespace ffcsync
