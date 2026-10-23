// Minimal "--key value" / "--flag" argument parsing shared by the executables.
#pragma once

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace ffcsync::cli {

class Args {
public:
    // `flags` are options that take no value.
    Args(int argc, char** argv, int first, std::set<std::string> flags = {}) {
        for (int i = first; i < argc; ++i) {
            std::string key = argv[i];
            if (key.rfind("--", 0) != 0) fail("unexpected argument '" + key + "'");
            key = key.substr(2);
            if (flags.contains(key)) {
                values_[key] = "1";
            } else {
                if (i + 1 >= argc) fail("missing value for --" + key);
                values_[key] = argv[++i];
            }
        }
    }

    bool has(const std::string& key) const { return values_.contains(key); }

    std::string get(const std::string& key, const std::string& fallback) const {
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }

    std::string require(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) fail("--" + key + " is required");
        return it->second;
    }

    long get_int(const std::string& key, long fallback) const {
        auto it = values_.find(key);
        if (it == values_.end()) return fallback;
        char* end = nullptr;
        long v = std::strtol(it->second.c_str(), &end, 10);
        if (*end != '\0') fail("--" + key + " expects an integer");
        return v;
    }

    [[noreturn]] static void fail(const std::string& msg) {
        std::cerr << "error: " << msg << "\n";
        std::exit(2);
    }

private:
    std::map<std::string, std::string> values_;
};

}  // namespace ffcsync::cli
