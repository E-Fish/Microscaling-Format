// util.hpp
// Small string, number and time helpers used by the rest of the program.
// Everything here is header-only (defined "inline") because the functions are short.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ppa {

// Remove spaces, tabs and line endings from both ends of a string.
inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Split on any whitespace: "a  b c" -> {"a", "b", "c"}.
inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

// Split on one character, keeping empty pieces: "a::b" -> {"a", "", "b"}.
inline std::vector<std::string> split_char(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string piece;
    std::istringstream in(s);
    while (std::getline(in, piece, sep)) out.push_back(piece);
    if (!s.empty() && s.back() == sep) out.push_back("");
    return out;
}

inline std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

inline bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "yes", "y", "true", "on", "1" (any case) count as yes.
inline bool is_yes(const std::string& v) {
    const std::string t = to_lower(trim(v));
    return t == "1" || t == "yes" || t == "y" || t == "true" || t == "on";
}

// True for a non-empty string of digits only, like "124".
inline bool is_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Parse a finite number. Returns std::nullopt for "NA", "", "inf", or anything that
// isn't entirely a number, so callers can't accidentally use a bad value.
inline std::optional<double> to_number(const std::string& s) {
    const std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    try {
        size_t used = 0;
        const double v = std::stod(t, &used);
        if (used != t.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

// Fixed decimals: fixed(3.14159, 2) -> "3.14".
inline std::string fixed(double v, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << v;
    return out.str();
}

// Shortest natural form: 2.0 -> "2", 4.5 -> "4.5" (same as Python's "{:g}").
inline std::string general(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

// Current local time formatted with strftime codes, e.g. "%Y-%m-%d %H:%M".
inline std::string now_str(const char* format) {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    char buf[64];
    std::strftime(buf, sizeof buf, format, &local);
    return buf;
}

// Left- or right-align text in a fixed-width column (for the summary table).
inline std::string pad_left(const std::string& s, size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}
inline std::string pad_right(const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

}  // namespace ppa
