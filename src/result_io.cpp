#include "ffcsync/result_io.hpp"

#include <algorithm>
#include <fstream>
#include <map>

#include "ffcsync/dataset.hpp"

namespace ffcsync {

namespace {

std::uint64_t parse_seq(const std::string& s) {
    try {
        return std::stoull(s);
    } catch (const std::exception&) {
        throw DataError("invalid seq '" + s + "'");
    }
}

RegistrationRequest request_from(const std::vector<std::string>& row, const std::string& source) {
    auto op = op_from_string(row[2]);
    if (!op) throw DataError(source + ": unknown op '" + row[2] + "'");
    return RegistrationRequest{row[1], *op, row[3], row[4]};
}

template <typename T>
std::string join(const T& items, char sep) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += sep;
        out += item;
    }
    return out;
}

}  // namespace

void write_results_csv(std::ostream& out, const std::vector<RegistrationResult>& results) {
    out << "seq,request_id,op,student_id,section_id,status,reason\n";
    for (const auto& r : results) {
        out << r.seq << ',' << r.request.request_id << ',' << to_string(r.request.op) << ','
            << r.request.student_id << ',' << r.request.section_id << ',' << to_string(r.status)
            << ',' << to_string(r.reason) << '\n';
    }
}

std::vector<RegistrationResult> read_results_csv(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw DataError("cannot open " + file.string());
    const std::string name = file.filename().string();
    std::vector<RegistrationResult> out;
    for (auto& row : read_csv(in, {"seq", "request_id", "op", "student_id", "section_id", "status",
                                   "reason"},
                              name)) {
        auto status = status_from_string(row[5]);
        auto reason = reason_from_string(row[6]);
        if (!status || !reason) throw DataError(name + ": bad status/reason");
        out.push_back(RegistrationResult{parse_seq(row[0]), request_from(row, name), *status, *reason});
    }
    return out;
}

void write_admission_csv(std::ostream& out, const std::vector<RegistrationResult>& admitted) {
    out << "seq,request_id,op,student_id,section_id\n";
    for (const auto& r : admitted) {
        out << r.seq << ',' << r.request.request_id << ',' << to_string(r.request.op) << ','
            << r.request.student_id << ',' << r.request.section_id << '\n';
    }
}

std::vector<RegistrationResult> read_admission_csv(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw DataError("cannot open " + file.string());
    const std::string name = file.filename().string();
    std::vector<RegistrationResult> out;
    for (auto& row : read_csv(in, {"seq", "request_id", "op", "student_id", "section_id"}, name)) {
        RegistrationResult r;
        r.seq = parse_seq(row[0]);
        r.request = request_from(row, name);
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
    return out;
}

void write_state_csv(std::ostream& out, const Catalog& catalog, const RegistrationState& state) {
    out << "kind,id,a,b,c\n";
    for (const auto& [id, section] : catalog.sections) {
        out << "section," << id << ',' << state.enrolled.at(id) << ',' << section.capacity << ",\n";
    }
    for (const auto& [id, st] : state.students) {
        std::vector<std::string> buckets;
        for (const auto& [b, c] : st.bucket_credits) buckets.push_back(b + "=" + std::to_string(c));
        out << "student," << id << ',' << st.credits << ',' << join(st.sections, ';') << ','
            << join(buckets, ';') << '\n';
    }
}

void write_summary(std::ostream& out, const std::vector<RegistrationResult>& results) {
    std::size_t accepted = 0;
    std::map<std::string_view, std::size_t> by_reason;
    for (const auto& r : results) {
        if (r.status == Status::Accepted) {
            ++accepted;
        } else {
            ++by_reason[to_string(r.reason)];
        }
    }
    out << "requests: " << results.size() << "\n"
        << "accepted: " << accepted << "\n"
        << "rejected: " << results.size() - accepted << "\n";
    for (const auto& [reason, count] : by_reason) out << "  " << reason << ": " << count << "\n";
}

void write_file(const std::filesystem::path& file,
                const std::function<void(std::ostream&)>& writer) {
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out) throw DataError("cannot write " + file.string());
    writer(out);
}

}  // namespace ffcsync
