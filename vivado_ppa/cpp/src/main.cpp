// main.cpp
// vivado_ppa (C++ version)
//
// Runs Vivado in batch mode (no GUI) on the designs you choose, at the clock
// period(s) you choose, and appends timing + area results to results.csv.
//
//   ./vivado_ppa                        interactive: asks what to run
//   ./vivado_ppa --list                 show designs and their variants
//   ./vivado_ppa mxfp6_comb --period 5 --note "baseline"
//   ./vivado_ppa mx_param --variants E2M3 E4M3 --period 2 --synth-only
//   ./vivado_ppa mx_param --period 2 --jobs 3       (3 Vivado runs at once)
//
// Reads the same settings.ini and designs/<name>/design.ini as vivado_ppa.py and
// writes the same results.csv columns, so the two versions can be used interchangeably.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "config.hpp"
#include "results.hpp"
#include "runner.hpp"
#include "util.hpp"

using namespace ppa;

namespace {

[[noreturn]] void die(const std::string& message) {
    std::cerr << "\nERROR: " << message << std::endl;
    std::exit(1);
}

// The folder this program lives in (designs/, settings.ini and results.csv sit next to it).
fs::path program_dir(const char* argv0) {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);   // Linux: the running binary
    if (!ec) return self.parent_path();
    return fs::absolute(argv0).parent_path();
}

// ---------------------------------------------------------------- command line

struct Args {
    std::vector<std::string> designs;
    std::vector<double> periods;
    std::vector<std::string> variants;
    std::string note;
    std::string part;
    std::string vivado;
    std::optional<int> jobs;
    bool synth_only = false;
    bool impl = false;
    bool checkpoint = false;
    bool list = false;
    bool dry_run = false;
};

const char* kHelp = R"(usage: vivado_ppa [designs ...] [options]

Run Vivado (batch, no GUI) on designs/<name>/ and log timing + area to results.csv.
Run with no arguments for interactive mode.

positional:
  designs                    design folder names in designs/ (or 'all')

options:
  -p, --period NS [NS ...]   clock period(s) in ns; several values = one run each
                             (default from settings.ini)
  -v, --variants LABEL ...   only run these variant labels
  -n, --note TEXT            note saved with every row
  -s, --synth-only           skip place and route (faster)
  -i, --impl                 force full implementation
  -j, --jobs N               Vivado runs at once (default from settings.ini)
      --part PART            override the FPGA part from settings.ini
      --vivado PATH          path to the vivado program
      --checkpoint           also save a .dcp checkpoint per run
      --list                 list designs and variants, then exit
      --dry-run              write the Vivado files but don't run Vivado
  -h, --help                 show this help
)";

bool looks_like_option(const std::string& s) {
    // "-3" is a (negative) number, not an option.
    return s.size() > 1 && s[0] == '-' && !to_number(s);
}

Args parse_args(int argc, char** argv) {
    Args a;
    std::vector<std::string> tokens(argv + 1, argv + argc);

    // Allow --option=value as well as --option value.
    std::vector<std::string> expanded;
    for (const auto& t : tokens) {
        const auto eq = t.find('=');
        if (starts_with(t, "--") && eq != std::string::npos) {
            expanded.push_back(t.substr(0, eq));
            expanded.push_back(t.substr(eq + 1));
        } else {
            expanded.push_back(t);
        }
    }
    tokens = expanded;

    auto need_value = [&](size_t& i, const std::string& opt) -> std::string {
        if (i + 1 >= tokens.size()) die(opt + " needs a value. See --help.");
        return tokens[++i];
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "-h" || t == "--help") {
            std::cout << kHelp;
            std::exit(0);
        } else if (t == "-p" || t == "--period") {
            while (i + 1 < tokens.size() && !looks_like_option(tokens[i + 1])) {
                const auto value = to_number(tokens[i + 1]);
                if (!value) break;                       // not a number: must be a design name
                a.periods.push_back(*value);
                ++i;
            }
            if (a.periods.empty()) die("--period needs one or more numbers, e.g. --period 2 or --period 4 5 6");
        } else if (t == "-v" || t == "--variants") {
            while (i + 1 < tokens.size() && !looks_like_option(tokens[i + 1])) {
                a.variants.push_back(tokens[++i]);
            }
            if (a.variants.empty()) die("--variants needs one or more labels, e.g. --variants E2M3 E4M3");
        } else if (t == "-n" || t == "--note") {
            a.note = need_value(i, t);
        } else if (t == "-j" || t == "--jobs") {
            const auto n = to_number(need_value(i, t));
            if (!n || *n < 1 || *n != static_cast<int>(*n)) die("--jobs needs a whole number, e.g. --jobs 2");
            a.jobs = static_cast<int>(*n);
        } else if (t == "--part") {
            a.part = need_value(i, t);
        } else if (t == "--vivado") {
            a.vivado = need_value(i, t);
        } else if (t == "-s" || t == "--synth-only") {
            a.synth_only = true;
        } else if (t == "-i" || t == "--impl") {
            a.impl = true;
        } else if (t == "--checkpoint") {
            a.checkpoint = true;
        } else if (t == "--list") {
            a.list = true;
        } else if (t == "--dry-run") {
            a.dry_run = true;
        } else if (looks_like_option(t)) {
            die("unknown option '" + t + "'. See --help.");
        } else {
            a.designs.push_back(t);
        }
    }
    return a;
}

// ---------------------------------------------------------------- interactive mode

std::string ask(const std::string& prompt, const std::string& fallback = "") {
    std::cout << prompt;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) answer.clear();   // Ctrl+D / end of input
    answer = trim(answer);
    return answer.empty() ? fallback : answer;
}

// Let the user pick items by number or name; "all" picks everything.
std::vector<std::string> pick(const std::vector<std::string>& items, const std::string& prompt) {
    for (size_t i = 0; i < items.size(); ++i) {
        std::cout << "  " << i + 1 << ". " << items[i] << "\n";
    }
    while (true) {
        std::string answer = ask(prompt + " (numbers or names separated by spaces, or 'all')", "all");
        if (to_lower(answer) == "all") return items;
        std::replace(answer.begin(), answer.end(), ',', ' ');

        std::vector<std::string> chosen;
        bool ok = true;
        for (const auto& token : split_ws(answer)) {
            std::string item;
            if (is_digits(token) && std::stoul(token) >= 1 && std::stoul(token) <= items.size()) {
                item = items[std::stoul(token) - 1];
            } else if (std::find(items.begin(), items.end(), token) != items.end()) {
                item = token;
            } else {
                std::cout << "  '" << token << "' is not on the list, try again.\n";
                ok = false;
                break;
            }
            if (std::find(chosen.begin(), chosen.end(), item) == chosen.end()) chosen.push_back(item);
        }
        if (ok && !chosen.empty()) return chosen;
    }
}

void interactive(Args& a, const Settings& s, const fs::path& designs_dir) {
    const auto names = list_designs(designs_dir);
    if (names.empty()) {
        die("No designs found in " + designs_dir.string() + ". Each design needs its own folder with a design.ini.");
    }
    std::cout << "\nDesigns:\n";
    a.designs = pick(names, "Which designs");

    std::vector<std::string> labels;
    for (const auto& name : a.designs) {
        for (const auto& v : load_design(designs_dir, name).variants) {
            if (std::find(labels.begin(), labels.end(), v.label) == labels.end()) labels.push_back(v.label);
        }
    }
    if (labels.size() > 1) {
        std::cout << "\nVariants in the chosen designs:\n";
        const auto chosen = pick(labels, "Which variants");
        if (chosen != labels) a.variants = chosen;
    }

    while (true) {
        std::string answer = ask("\nClock period(s) in ns, e.g. 5 or 4 5 6", s.period_ns);
        std::replace(answer.begin(), answer.end(), ',', ' ');
        a.periods.clear();
        bool ok = true;
        for (const auto& token : split_ws(answer)) {
            const auto v = to_number(token);
            if (!v || *v <= 0) { ok = false; break; }
            a.periods.push_back(*v);
        }
        if (ok && !a.periods.empty()) break;
        std::cout << "  Enter positive numbers, like 5 or 4.5 5 6\n";
    }

    const std::string impl_default = is_yes(s.implementation) ? "i" : "s";
    const std::string mode = ask("Synthesis only (s, fast) or full implementation (i, accurate timing)", impl_default);
    a.synth_only = starts_with(to_lower(mode), "s");
    a.note = ask("Note describing this run (what changed)");
    const auto jobs = to_number(ask("How many Vivado runs at once", s.jobs));
    a.jobs = jobs && *jobs >= 1 ? static_cast<int>(*jobs) : 1;
}

// ---------------------------------------------------------------- --list

void print_list(const fs::path& designs_dir) {
    const auto names = list_designs(designs_dir);
    if (names.empty()) std::cout << "No designs found in " << designs_dir.string() << "\n";
    for (const auto& name : names) {
        const Design d = load_design(designs_dir, name);
        std::vector<std::string> files;
        for (const auto& s : d.sources) files.push_back(s.filename().string());
        std::cout << "\n" << name << "   (top: " << d.top << ", files: " << join(files, " ") << ")\n";
        for (const auto& v : d.variants) {
            std::vector<std::string> g;
            for (const auto& [k, x] : v.generics) g.push_back(k + "=" + x);
            std::cout << "    " << pad_right(v.label, 12) << " " << join(g, " ") << "\n";
        }
    }
}

// ---------------------------------------------------------------- summary table

std::string table_line(const std::string& design, const std::string& variant, const std::string& period,
                       const std::string& wns, const std::string& fmax, const std::string& luts,
                       const std::string& ffs, const std::string& dsps, const std::string& status) {
    return pad_right(design.substr(0, 14), 14) + " " + pad_right(variant.substr(0, 10), 10) + " " +
           pad_left(period, 7) + " " + pad_left(wns, 8) + " " + pad_left(fmax, 9) + " " +
           pad_left(luts, 7) + " " + pad_left(ffs, 7) + " " + pad_left(dsps, 6) + "  " + status;
}

}  // namespace

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    try {
        const fs::path root = program_dir(argv[0]);
        const fs::path designs_dir = root / "designs";
        const Settings settings = load_settings(root);
        Args args = parse_args(argc, argv);

        if (args.list) {
            print_list(designs_dir);
            return 0;
        }

        if (args.designs.empty()) interactive(args, settings, designs_dir);
        if (args.designs.size() == 1 && args.designs[0] == "all") args.designs = list_designs(designs_dir);

        // ----- options -----
        const std::string part = !args.part.empty() ? args.part : settings.part;
        if (part.empty()) die("No FPGA part set. Put it in settings.ini (part = ...) or pass --part.");

        bool impl = is_yes(settings.implementation);
        if (args.synth_only) impl = false;
        if (args.impl) impl = true;

        std::vector<double> periods = args.periods;
        if (periods.empty()) {
            const auto p = to_number(settings.period_ns);
            if (!p) die("period_ns in settings.ini isn't a number: '" + settings.period_ns + "'");
            periods.push_back(*p);
        }
        for (double p : periods) {
            if (p <= 0) die("Clock periods must be positive.");
        }

        // ----- build the job list (designs are kept alive here; jobs point into them) -----
        std::vector<Design> designs;
        designs.reserve(args.designs.size());
        for (const auto& name : args.designs) designs.push_back(load_design(designs_dir, name));

        std::vector<Job> jobs;
        for (const Design& d : designs) {
            std::vector<const Variant*> chosen;
            for (const Variant& v : d.variants) {
                if (args.variants.empty() ||
                    std::find(args.variants.begin(), args.variants.end(), v.label) != args.variants.end()) {
                    chosen.push_back(&v);
                }
            }
            if (!args.variants.empty() && chosen.empty()) {
                say("NOTE: " + d.name + " has none of the variants " + join(args.variants, " ") + "; skipping it.");
            }
            for (const Variant* v : chosen) {
                for (double p : periods) jobs.push_back({&d, v, p});
            }
        }
        if (jobs.empty()) die("Nothing to run. Check the design and variant names with:  ./vivado_ppa --list");

        const std::string wanted = !args.vivado.empty() ? args.vivado : settings.vivado;
        const std::string vivado = find_vivado(wanted);
        if (vivado.empty() && !args.dry_run) {
            die("Can't find the vivado program ('" + wanted + "').\n"
                "  Fix: open settings.ini and set 'path' to the full path of vivado.\n"
                "  To find it, open the Vivado GUI once and type in its Tcl Console:\n"
                "      puts $env(XILINX_VIVADO)/bin/vivado\n"
                "  and paste what it prints into settings.ini.");
        }

        Options opts;
        opts.vivado = vivado;
        opts.part = part;
        opts.impl = impl;
        opts.synth_directive = settings.synth_directive;
        opts.impl_directive = settings.impl_directive;
        opts.checkpoint = args.checkpoint || is_yes(settings.save_checkpoint);
        opts.note = args.note;
        opts.dry_run = args.dry_run;
        opts.commit = git_commit(root);
        opts.license = settings.license;

        int n_threads = args.jobs ? *args.jobs : 1;
        if (!args.jobs) {
            const auto j = to_number(settings.jobs);
            if (j && *j >= 1) n_threads = static_cast<int>(*j);
        }
        n_threads = std::clamp(n_threads, 1, 64);

        ResultsCsv csv(root / "results.csv");

        std::vector<std::string> period_text;
        for (double p : periods) period_text.push_back(general(p));
        say("\nRunning " + std::to_string(jobs.size()) + " job(s): " +
            (impl ? "synth + place + route" : "synthesis only") + ", part " + part + ", " +
            std::to_string(n_threads) + " at a time");
        say("Clock period(s): " + join(period_text, ", ") + " ns");
        say("Results go to " + csv.path().string() + "\n");

        fs::create_directories(root / "runs");
        if (!args.dry_run) {
            const std::string moved = csv.ensure_header();
            if (!moved.empty()) say("NOTE: results.csv had different columns; moved it to " + moved);
        }

        // ----- run the jobs on a small pool of worker threads -----
        install_interrupt_handler();
        std::vector<Row> rows(jobs.size());
        std::atomic<size_t> next{0};
        const auto t0 = std::chrono::steady_clock::now();

        auto worker = [&]() {
            while (true) {
                const size_t i = next++;                 // each thread grabs the next unclaimed job
                if (i >= jobs.size()) return;
                try {
                    rows[i] = run_job(jobs[i], opts, static_cast<int>(i + 1), static_cast<int>(jobs.size()), root, csv);
                } catch (const std::exception& e) {
                    // Something went wrong outside Vivado (e.g. a file couldn't be written).
                    say("[" + std::to_string(i + 1) + "/" + std::to_string(jobs.size()) + "] FAILED: " + e.what());
                    Row r;
                    r["design"] = jobs[i].design->name;
                    r["variant"] = jobs[i].variant->label;
                    r["period_ns"] = general(jobs[i].period);
                    for (const char* k : {"wns_ns", "fmax_mhz", "luts", "ffs", "dsps"}) r[k] = "NA";
                    r["status"] = "FAILED";
                    rows[i] = r;
                }
            }
        };
        std::vector<std::thread> pool;
        const int workers = std::min<int>(n_threads, static_cast<int>(jobs.size()));
        for (int t = 0; t < workers; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        // ----- summary -----
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
            if (x.at("design") != y.at("design")) return x.at("design") < y.at("design");
            if (x.at("variant") != y.at("variant")) return x.at("variant") < y.at("variant");
            return std::stod(x.at("period_ns")) < std::stod(y.at("period_ns"));
        });
        const std::string rule(92, '=');
        say("\n" + rule);
        say(table_line("design", "variant", "period", "WNS", "Fmax", "LUTs", "FFs", "DSPs", "status"));
        int failed = 0;
        for (const Row& r : rows) {
            say(table_line(r.at("design"), r.at("variant"), r.at("period_ns"), r.at("wns_ns"), r.at("fmax_mhz"),
                           r.at("luts"), r.at("ffs"), r.at("dsps"), r.at("status")));
            if (r.at("status") == "FAILED") ++failed;
        }
        say(rule);
        say(std::to_string(rows.size()) + " run(s) in " + fixed(elapsed, 0) + "s, " + std::to_string(failed) +
            " failed. Results: " + csv.path().string());
        return failed ? 1 : 0;

    } catch (const std::exception& e) {
        die(e.what());
    }
}
