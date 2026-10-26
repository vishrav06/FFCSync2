// Loading the catalog and request files from plain CSV.
//
// CSV dialect (kept deliberately simple, see README.md):
//   * the first non-blank line is a header and is checked against the expected columns
//   * fields are separated by ',' and trimmed; quoting is not supported
//   * list-valued fields (slots, prerequisites, completed courses) use ';'
//   * blank lines and lines starting with '#' are ignored
#pragma once

#include <filesystem>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ffcsync/model.hpp"

namespace ffcsync {

class DataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::vector<std::string> split(std::string_view s, char sep);
std::string trim(std::string_view s);

// Reads a CSV stream into rows, validating the header. Throws DataError.
std::vector<std::vector<std::string>> read_csv(std::istream& in,
                                               const std::vector<std::string>& header,
                                               const std::string& source_name);

// Loads config.csv, buckets.csv, courses.csv, sections.csv and students.csv
// from `dir`, cross-checking references. Throws DataError on any problem.
Catalog load_catalog(const std::filesystem::path& dir);

// Request file: request_id,op,student_id,section_id
std::vector<RegistrationRequest> load_requests(const std::filesystem::path& file);
std::vector<RegistrationRequest> read_requests(std::istream& in, const std::string& source_name);

}  // namespace ffcsync
