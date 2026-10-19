// config.cpp
// Reading settings.ini and designs/<name>/design.ini.
#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "util.hpp"

namespace ppa {

namespace {

// Cut off an inline comment: a '#' or ';' that has a space or tab right before it.
std::string strip_inline_comment(const std::string& s) {
    for (size_t i = 1; i < s.size(); ++i) {
        if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

const IniFile::Items kNoItems;

}  // namespace

// ---------------------------------------------------------------- IniFile

bool IniFile::load(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return false;

    std::string line;
    std::string section;
    bool have_key = false;     // a key has been read in the current section
    size_t last_index = 0;     // position of that key (for continuation lines)
    int lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();   // Windows line endings
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;       // blank or comment line

        const bool indented = std::isspace(static_cast<unsigned char>(line[0])) != 0;
        if (indented && have_key) {
            // An indented line continues the previous value (same rule as Python's configparser).
            sections_[section][last_index].second += "\n" + trim(strip_inline_comment(t));
            continue;
        }

        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            if (sections_.find(section) == sections_.end()) {
                order_.push_back(section);
                sections_[section];   // create it, even if empty
            }
            have_key = false;
            continue;
        }

        const std::string where = path.string() + ": line " + std::to_string(lineno);
        if (section.empty()) throw std::runtime_error(where + " is outside any [section]");

        const auto eq = t.find('=');
        if (eq == std::string::npos) throw std::runtime_error(where + ": expected  key = value");

        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(strip_inline_comment(t.substr(eq + 1)));

        Items& items = sections_[section];
        auto it = std::find_if(items.begin(), items.end(),
                               [&](const auto& kv) { return kv.first == key; });
        if (it != items.end()) {
            it->second = value;                         // repeated key: last one wins
            last_index = static_cast<size_t>(it - items.begin());
        } else {
            items.emplace_back(key, value);
            last_index = items.size() - 1;
        }
        have_key = true;
    }
    return true;
}

bool IniFile::has_section(const std::string& section) const {
    return sections_.find(section) != sections_.end();
}

const IniFile::Items& IniFile::items(const std::string& section) const {
    auto it = sections_.find(section);
    return it == sections_.end() ? kNoItems : it->second;
}

std::string IniFile::get(const std::string& section, const std::string& key,
                         const std::string& fallback) const {
    for (const auto& [k, v] : items(section)) {
        if (k == key) return v;
    }
    return fallback;
}

// ---------------------------------------------------------------- settings.ini

Settings load_settings(const fs::path& root) {
    Settings s;
    IniFile ini;
    if (!ini.load(root / "settings.ini")) return s;   // no file: keep defaults

    // Keys may appear under any section; "path" is the vivado program.
    for (const auto& section : ini.section_names()) {
        for (const auto& [key, raw] : ini.items(section)) {
            const std::string v = trim(raw);
            if (key == "path" || key == "vivado")    s.vivado = v;
            else if (key == "part")                   s.part = v;
            else if (key == "period_ns")              s.period_ns = v;
            else if (key == "implementation")         s.implementation = v;
            else if (key == "synth_directive")        s.synth_directive = v;
            else if (key == "impl_directive")         s.impl_directive = v;
            else if (key == "jobs")                   s.jobs = v;
            else if (key == "save_checkpoint")        s.save_checkpoint = v;
            else if (key == "license")                s.license = v;
        }
    }
    return s;
}

// ---------------------------------------------------------------- designs

std::vector<std::string> list_designs(const fs::path& designs_dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::is_directory(designs_dir, ec)) return names;

    for (const auto& entry : fs::directory_iterator(designs_dir)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        if (fs::is_regular_file(entry.path() / "design.ini", ec)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Design load_design(const fs::path& designs_dir, const std::string& name) {
    const fs::path ddir = designs_dir / name;
    const fs::path ini_path = ddir / "design.ini";
    const std::string where = ini_path.string();

    std::error_code ec;
    if (!fs::is_regular_file(ini_path, ec)) {
        std::vector<std::string> have = list_designs(designs_dir);
        throw std::runtime_error("No design called '" + name + "' (looked for " + where +
                                 ").\nAvailable designs: " +
                                 (have.empty() ? "(none)" : join(have, ", ")));
    }

    IniFile ini;
    ini.load(ini_path);
    if (!ini.has_section("design") || !ini.has_section("variants")) {
        throw std::runtime_error(where + " needs a [design] section and a [variants] section.");
    }

    Design d;
    d.name = name;
    d.dir = ddir;

    d.top = trim(ini.get("design", "top"));
    const std::string sources = trim(ini.get("design", "sources"));
    if (d.top.empty()) throw std::runtime_error(where + ": 'top' is missing under [design].");
    if (sources.empty()) throw std::runtime_error(where + ": 'sources' is missing under [design].");

    for (const auto& file : split_ws(sources)) {
        const fs::path p = ddir / file;
        if (!fs::is_regular_file(p, ec)) {
            throw std::runtime_error(where + ": source file '" + file + "' not found in " +
                                     ddir.string());
        }
        d.sources.push_back(p);
    }

    d.clock = trim(ini.get("design", "clock", "clk"));
    d.ii = trim(ini.get("design", "ii", "1"));
    d.elements = trim(ini.get("design", "elements", ""));
    d.io_false_path = is_yes(ini.get("design", "io_false_path", "yes"));
    const std::string latency = trim(ini.get("design", "latency", "2"));

    for (const auto& [label, spec] : ini.items("variants")) {
        Variant v;
        v.label = label;
        v.latency = latency;
        for (const auto& token : split_ws(spec)) {
            const auto eq = token.find('=');
            if (eq == std::string::npos) {
                throw std::runtime_error(where + ": variant '" + label + "' has '" + token +
                                         "'; expected PARAM=value");
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (to_lower(key) == "latency") {
                v.latency = value;                      // latency=X overrides the design default
            } else {
                v.generics.emplace_back(key, value);    // everything else goes to Vivado
            }
        }
        d.variants.push_back(v);
    }
    if (d.variants.empty()) throw std::runtime_error(where + ": [variants] is empty.");

    return d;
}

}  // namespace ppa
