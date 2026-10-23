// Plain-text outputs: result files, final-state files, admission logs and
// the human-readable summary. All writers produce deterministic output
// (rows sorted by seq or by id) so files can be compared with `diff`.
#pragma once

#include <filesystem>
#include <functional>
#include <ostream>
#include <vector>

#include "ffcsync/model.hpp"

namespace ffcsync {

// seq,request_id,op,student_id,section_id,status,reason
void write_results_csv(std::ostream& out, const std::vector<RegistrationResult>& results);
std::vector<RegistrationResult> read_results_csv(const std::filesystem::path& file);

// seq,request_id,op,student_id,section_id  (the order in which requests were admitted)
void write_admission_csv(std::ostream& out, const std::vector<RegistrationResult>& admitted);
// Returns requests with their seq (status/reason left at defaults), sorted by seq.
std::vector<RegistrationResult> read_admission_csv(const std::filesystem::path& file);

// kind,id,a,b,c
//   section,<section_id>,<enrolled>,<capacity>,
//   student,<student_id>,<credits_tenths>,<sections ;-separated>,<bucket=tenths ;-separated>
void write_state_csv(std::ostream& out, const Catalog& catalog, const RegistrationState& state);

// Counts per status and per reason.
void write_summary(std::ostream& out, const std::vector<RegistrationResult>& results);

// Creates parent directories, opens `file` and passes the stream to `writer`.
void write_file(const std::filesystem::path& file,
                const std::function<void(std::ostream&)>& writer);

}  // namespace ffcsync
