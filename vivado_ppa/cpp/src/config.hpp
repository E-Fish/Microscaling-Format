// config.hpp
// Reading settings.ini and designs/<name>/design.ini.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ppa {

namespace fs = std::filesystem;

// A minimal INI reader:
//   [section]
//   key = value      # comments after a space are ignored
// Keys keep their original case and file order (variant labels like E2M3 depend on both).
class IniFile {
public:
    using Items = std::vector<std::pair<std::string, std::string>>;

    // Returns false if the file can't be opened. Throws std::runtime_error on bad syntax.
    bool load(const fs::path& path);

    bool has_section(const std::string& section) const;
    const Items& items(const std::string& section) const;
    std::vector<std::string> section_names() const { return order_; }

    // Value of key in section, or fallback if the key isn't there.
    std::string get(const std::string& section, const std::string& key,
                    const std::string& fallback = "") const;

private:
    std::vector<std::string> order_;          // section names in file order
    std::map<std::string, Items> sections_;   // section name -> its key/value pairs
};

// Everything in settings.ini (all stored as text, converted where used).
struct Settings {
    std::string vivado = "vivado";
    std::string part;
    std::string period_ns = "10";
    std::string implementation = "yes";
    std::string synth_directive = "default";
    std::string impl_directive = "Default";
    std::string jobs = "1";
    std::string save_checkpoint = "no";
    std::string license;
};

Settings load_settings(const fs::path& root);

// One line under [variants], e.g.  E2M3 = EXP=2 MAN=3 N=32
struct Variant {
    std::string label;
    std::vector<std::pair<std::string, std::string>> generics;  // {"EXP","2"}, {"MAN","3"}, ...
    std::string latency;
};

// One folder in designs/.
struct Design {
    std::string name;
    fs::path dir;
    std::string top;
    std::vector<fs::path> sources;
    std::string clock = "clk";
    std::string ii = "1";
    std::string elements;
    bool io_false_path = true;
    std::vector<Variant> variants;
};

// Names of the folders in designs/ that have a design.ini (skipping _template etc.).
std::vector<std::string> list_designs(const fs::path& designs_dir);

// Load and check one design. Throws std::runtime_error with a readable message on problems.
Design load_design(const fs::path& designs_dir, const std::string& name);

}  // namespace ppa
