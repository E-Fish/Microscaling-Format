// results.cpp
// Reading Vivado's output files and writing results.csv.
#include "results.hpp"

#include <fstream>
#include <regex>
#include <sstream>

#include "util.hpp"

namespace ppa {

const std::vector<std::string> kColumns = {
    "date", "design", "variant", "EXP", "MAN", "N", "STAGES", "generics",
    "period_ns", "target_mhz", "stage", "status",
    "wns_ns", "whs_ns", "fmax_mhz", "timing_met",
    "luts", "ffs", "dsps", "bram", "carry",
    "latency_cycles", "ii", "elements", "gelem_s", "luts_per_elem", "melem_s_per_lut",
    "logic_levels", "logic_delay_ns", "net_delay_ns", "crit_start", "crit_end",
    "part", "vivado", "synth_directive", "impl_directive", "runtime_s",
    "commit", "run_dir", "note", "error",
};

namespace {

std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// Quote a CSV field only when needed (commas, quotes, line breaks), doubling any quotes.
std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

}  // namespace

// ---------------------------------------------------------------- Vivado output

std::map<std::string, std::string> read_metrics(const fs::path& path) {
    std::map<std::string, std::string> m;
    for (const auto& line : read_lines(path)) {
        if (line.empty()) continue;
        const auto space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string value = space == std::string::npos ? "" : trim(line.substr(space + 1));
        m[key] = value;
    }
    return m;
}

std::map<std::string, std::string> parse_util(const fs::path& path) {
    std::map<std::string, std::string> u = {{"luts", "NA"}, {"ffs", "NA"}, {"dsps", "NA"}, {"bram", "NA"}};

    // Report wording differs by chip family: 7-series ("Slice ..."), UltraScale ("CLB ..."),
    // Versal. Each value has a list of patterns tried in order; the first match wins.
    static const std::vector<std::pair<std::string, std::vector<std::regex>>> patterns = {
        {"luts", {std::regex(R"(^\|\s*(?:Slice |CLB )?LUTs\*?\s*\|\s*(\d+))")}},
        {"ffs",  {std::regex(R"(^\|\s*(?:Slice |CLB )?Registers\*?\s*\|\s*(\d+))"),
                  std::regex(R"(^\|\s*Register as Flip Flop\s*\|\s*(\d+))")}},
        {"dsps", {std::regex(R"(^\|\s*(?:DSPs|DSP Slices|DSP Engines)\*?\s*\|\s*(\d+))")}},
        {"bram", {std::regex(R"(^\|\s*Block RAM Tile\*?\s*\|\s*([\d.]+))")}},
    };

    const std::vector<std::string> lines = read_lines(path);
    for (const auto& [key, regexes] : patterns) {
        bool found = false;
        for (const auto& re : regexes) {
            for (const auto& line : lines) {
                std::smatch m;
                if (std::regex_search(line, m, re)) {
                    u[key] = m[1].str();
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }
    return u;
}

std::string log_errors(const fs::path& run_dir, int limit) {
    const fs::path log_path = run_dir / "vivado.log";
    std::error_code ec;

    if (!fs::exists(log_path, ec)) {
        // Vivado quit before writing its log; show the last things it printed instead.
        std::vector<std::string> printed;
        for (const auto& line : read_lines(run_dir / "stdout.txt")) {
            if (!trim(line).empty()) printed.push_back(trim(line));
        }
        if (printed.empty()) {
            return "Vivado did not start and printed nothing (check the path in settings.ini)";
        }
        const size_t from = printed.size() > static_cast<size_t>(limit) ? printed.size() - limit : 0;
        return "Vivado quit before starting: " +
               join(std::vector<std::string>(printed.begin() + from, printed.end()), " | ");
    }

    // Prefer ERROR lines; fall back to CRITICAL WARNING lines.
    std::vector<std::string> errors, criticals;
    for (const auto& line : read_lines(log_path)) {
        if (starts_with(line, "ERROR:")) errors.push_back(trim(line));
        else if (starts_with(line, "CRITICAL WARNING:")) criticals.push_back(trim(line));
    }
    std::vector<std::string>& pick = errors.empty() ? criticals : errors;
    if (pick.empty()) return "unknown failure, see vivado.log";
    if (pick.size() > static_cast<size_t>(limit)) pick.resize(limit);
    return join(pick, " | ");
}

// ---------------------------------------------------------------- results.csv

std::string ResultsCsv::ensure_header() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string moved_to;
    std::error_code ec;

    if (fs::exists(path_, ec)) {
        std::ifstream in(path_);
        std::string first;
        std::getline(in, first);
        if (!first.empty() && first.back() == '\r') first.pop_back();
        if (first == join(kColumns, ",")) return "";   // already good

        // Different columns: move the old file aside rather than mixing formats.
        const fs::path old = path_.parent_path() / ("results_old_" + now_str("%Y%m%d_%H%M%S") + ".csv");
        fs::rename(path_, old);
        moved_to = old.filename().string();
    }

    std::ofstream out(path_, std::ios::binary);
    out << join(kColumns, ",") << "\r\n";   // CRLF, same as Python's csv module
    return moved_to;
}

void ResultsCsv::append(const Row& row) {
    std::lock_guard<std::mutex> lock(mutex_);   // one thread writes at a time
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    for (size_t i = 0; i < kColumns.size(); ++i) {
        if (i) out << ',';
        auto it = row.find(kColumns[i]);
        out << csv_field(it == row.end() ? "" : it->second);
    }
    out << "\r\n";
}

}  // namespace ppa
