// Shared helpers for the test-suite.
#pragma once

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/json.h"
#include "model/account.h"

namespace ngtest {

inline std::filesystem::path temp_dir(const std::string& prefix) {
    std::filesystem::path base = std::filesystem::temp_directory_path() / ("ngtest-" + prefix + "-XXXXXX");
    std::string tmpl = base.string();
    char* made = mkdtemp(tmpl.data());
    REQUIRE(made != nullptr);
    return std::filesystem::path(made);
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& prefix) : path(temp_dir(prefix)) {}
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return (path / name).string(); }
};

inline void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

inline ng::AccountMeta meta(const std::string& id, int num, const std::string& broker = "Tiger", const std::string& medium = "API") {
    ng::AccountMeta m;
    m.id = id;
    m.num_id = num;
    m.broker = broker;
    m.trading_medium = medium;
    return m;
}

inline ng::AccountDirectory directory(std::initializer_list<ng::AccountMeta> metas) {
    return ng::AccountDirectory(std::vector<ng::AccountMeta>(metas));
}

}  // namespace ngtest
