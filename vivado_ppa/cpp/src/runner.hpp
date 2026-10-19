// runner.hpp
// Running one Vivado job: writing its constraint and Tcl files, starting Vivado,
// and turning its output into a results.csv row.
#pragma once

#include <filesystem>
#include <string>

#include "config.hpp"
#include "results.hpp"

namespace ppa {

namespace fs = std::filesystem;

// Options shared by every job in one invocation.
struct Options {
    std::string vivado;           // full path to the vivado program
    std::string part;
    std::string synth_directive;
    std::string impl_directive;
    std::string note;
    std::string commit;           // git commit of the folder, or "nogit"
    std::string license;          // optional license server
    bool impl = true;             // false = synthesis only
    bool checkpoint = false;      // also write design.dcp
    bool dry_run = false;         // write files but don't start Vivado
};

// One design + variant + clock period.
struct Job {
    const Design* design;
    const Variant* variant;
    double period;
};

// Print one line, without lines from different threads getting mixed together.
void say(const std::string& line);

// Full path of the vivado program, or "" if it can't be found.
std::string find_vivado(const std::string& name_or_path);

// Short git commit of `folder` ("-dirty" if it has uncommitted changes), or "nogit".
std::string git_commit(const fs::path& folder);

// Make Ctrl+C stop any running Vivado jobs before exiting.
void install_interrupt_handler();

// Run one job from start to finish. Appends a row to csv (unless it's a dry run)
// and returns the row for the summary table.
Row run_job(const Job& job, const Options& opts, int index, int total,
            const fs::path& root, ResultsCsv& csv);

}  // namespace ppa
