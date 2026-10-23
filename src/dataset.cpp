#include "ffcsync/dataset.hpp"

#include <charconv>
#include <fstream>

namespace ffcsync {

namespace {

std::ifstream open_file(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw DataError("cannot open " + file.string());
    return in;
}

int parse_int(const std::string& s, const std::string& what) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        throw DataError("invalid integer for " + what + ": '" + s + "'");
    }
    return value;
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    for (auto& item : split(s, ';')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(sep, start);
        out.push_back(trim(s.substr(start, pos == std::string_view::npos ? s.npos : pos - start)));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string trim(std::string_view s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

std::vector<std::vector<std::string>> read_csv(std::istream& in,
                                               const std::vector<std::string>& header,
                                               const std::string& source_name) {
    std::vector<std::vector<std::string>> rows;
    std::string line;
    bool seen_header = false;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        auto fields = split(t, ',');
        if (!seen_header) {
            if (fields != header) throw DataError(source_name + ": unexpected header '" + t + "'");
            seen_header = true;
            continue;
        }
        if (fields.size() != header.size()) {
            throw DataError(source_name + ":" + std::to_string(line_no) + ": expected " +
                            std::to_string(header.size()) + " fields, got " +
                            std::to_string(fields.size()));
        }
        rows.push_back(std::move(fields));
    }
    if (!seen_header) throw DataError(source_name + ": missing header");
    return rows;
}

Catalog load_catalog(const std::filesystem::path& dir) {
    Catalog cat;

    {
        auto in = open_file(dir / "config.csv");
        for (auto& row : read_csv(in, {"key", "value"}, "config.csv")) {
            if (row[0] == "window_open") {
                cat.config.window_open = parse_int(row[1], "window_open") != 0;
            } else if (row[0] == "max_credits_tenths") {
                cat.config.max_credits = parse_int(row[1], "max_credits_tenths");
            } else {
                throw DataError("config.csv: unknown key '" + row[0] + "'");
            }
        }
    }
    {
        auto in = open_file(dir / "buckets.csv");
        for (auto& row : read_csv(in, {"bucket", "max_tenths"}, "buckets.csv")) {
            cat.config.bucket_limits[row[0]] = parse_int(row[1], "bucket " + row[0]);
        }
    }
    {
        auto in = open_file(dir / "courses.csv");
        for (auto& row : read_csv(in, {"course_code", "title", "credits_tenths", "bucket", "prereqs"},
                                  "courses.csv")) {
            Course c{row[0], row[1], parse_int(row[2], "credits of " + row[0]), row[3],
                     split_list(row[4])};
            if (!cat.courses.emplace(c.code, c).second) {
                throw DataError("courses.csv: duplicate course " + c.code);
            }
        }
        for (const auto& [code, course] : cat.courses) {
            for (const auto& p : course.prereqs) {
                if (!cat.courses.contains(p)) {
                    throw DataError("courses.csv: " + code + " has unknown prerequisite " + p);
                }
            }
        }
    }
    {
        auto in = open_file(dir / "sections.csv");
        for (auto& row : read_csv(in, {"section_id", "course_code", "faculty", "slots", "capacity"},
                                  "sections.csv")) {
            Section s{row[0], row[1], row[2], split_list(row[3]),
                      parse_int(row[4], "capacity of " + row[0])};
            if (!cat.courses.contains(s.course)) {
                throw DataError("sections.csv: " + s.id + " references unknown course " + s.course);
            }
            if (s.capacity < 0) throw DataError("sections.csv: negative capacity for " + s.id);
            if (!cat.sections.emplace(s.id, s).second) {
                throw DataError("sections.csv: duplicate section " + s.id);
            }
        }
    }
    {
        auto in = open_file(dir / "students.csv");
        for (auto& row : read_csv(in, {"student_id", "name", "completed_courses"}, "students.csv")) {
            auto completed = split_list(row[2]);
            Student s{row[0], row[1], {completed.begin(), completed.end()}};
            if (!cat.students.emplace(s.id, s).second) {
                throw DataError("students.csv: duplicate student " + s.id);
            }
        }
    }
    return cat;
}

std::vector<RegistrationRequest> read_requests(std::istream& in, const std::string& source_name) {
    std::vector<RegistrationRequest> out;
    for (auto& row : read_csv(in, {"request_id", "op", "student_id", "section_id"}, source_name)) {
        auto op = op_from_string(row[1]);
        if (!op) throw DataError(source_name + ": unknown op '" + row[1] + "'");
        if (row[0].empty()) throw DataError(source_name + ": empty request_id");
        out.push_back(RegistrationRequest{row[0], *op, row[2], row[3]});
    }
    return out;
}

std::vector<RegistrationRequest> load_requests(const std::filesystem::path& file) {
    auto in = open_file(file);
    return read_requests(in, file.filename().string());
}

}  // namespace ffcsync
