// results.hpp
// Reading Vivado's output files and writing results.csv.
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ppa {

namespace fs = std::filesystem;

// One results.csv row: column name -> value.
using Row = std::map<std::string, std::string>;

// The results.csv columns, in order. Identical to the Python version, so rows from
// either version line up in the same file.
extern const std::vector<std::string> kColumns;

// metrics.txt is written by the generated Tcl as "key value" lines.
std::map<std::string, std::string> read_metrics(const fs::path& path);

// Pull LUTs / FFs / DSPs / BRAM out of Vivado's util.rpt. Missing values are "NA".
std::map<std::string, std::string> parse_util(const fs::path& path);

// A short, readable reason a run failed, taken from vivado.log (or stdout.txt if
// Vivado quit before it could write a log).
std::string log_errors(const fs::path& run_dir, int limit = 3);

// results.csv, safe to append to from several threads at once.
class ResultsCsv {
public:
    explicit ResultsCsv(fs::path path) : path_(std::move(path)) {}

    // Create the file with a header if needed. If an existing file has different
    // columns, it is renamed out of the way; returns that new name ("" otherwise).
    std::string ensure_header();

    void append(const Row& row);
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    std::mutex mutex_;
};

}  // namespace ppa
