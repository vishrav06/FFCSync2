// Shared helpers for the test suite.
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ffcsync/dataset.hpp"
#include "ffcsync/model.hpp"

namespace ffcsync::test {

inline std::filesystem::path source_dir() { return FFCSYNC_SOURCE_DIR; }
inline std::filesystem::path data_dir() { return source_dir() / "data"; }

// The documented example dataset in data/ (loaded once, never modified).
inline const Catalog& catalog() {
    static const Catalog cat = load_catalog(data_dir());
    return cat;
}

inline RegistrationRequest add(std::string student, std::string section, std::string id = "t") {
    return {std::move(id), Op::Add, std::move(student), std::move(section)};
}

inline RegistrationRequest drop(std::string student, std::string section, std::string id = "t") {
    return {std::move(id), Op::Drop, std::move(student), std::move(section)};
}

inline std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace ffcsync::test
