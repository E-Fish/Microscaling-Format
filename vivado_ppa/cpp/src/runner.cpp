// runner.cpp
// Running one Vivado job: writing its constraint and Tcl files, starting Vivado,
// and turning its output into a results.csv row.
#include "runner.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.hpp"

extern char** environ;   // the current process's environment variables (POSIX)

namespace ppa {

namespace {

const std::string kNA = "NA";

// ---------------------------------------------------------------- running processes
//
// Each Vivado run is a child process. We remember their IDs so Ctrl+C can stop them.
// A fixed array of atomics is used because the Ctrl+C handler may only touch
// "async-signal-safe" things: no locks, no memory allocation.

constexpr int kMaxProcs = 64;
std::atomic<pid_t> g_procs[kMaxProcs];

void remember_proc(pid_t pid) {
    for (auto& slot : g_procs) {
        pid_t empty = 0;
        if (slot.compare_exchange_strong(empty, pid)) return;
    }
}

void forget_proc(pid_t pid) {
    for (auto& slot : g_procs) {
        pid_t expected = pid;
        if (slot.compare_exchange_strong(expected, 0)) return;
    }
}

extern "C" void on_interrupt(int) {
    const char msg[] = "\nStopping: killing running Vivado jobs...\n";
    ssize_t ignored = write(STDOUT_FILENO, msg, sizeof msg - 1);
    (void)ignored;
    for (auto& slot : g_procs) {
        const pid_t pid = slot.load();
        if (pid > 0) kill(-pid, SIGTERM);   // negative pid = the whole process group
    }
    _exit(130);
}

// Start Vivado in run_dir and wait for it. Returns its exit code (or 128+signal).
int run_vivado(const fs::path& run_dir, const Options& opts) {
    // Build everything the child needs *before* fork(): after fork() in a program with
    // threads, the child may only call simple system functions until exec.
    std::vector<std::string> args = {opts.vivado, "-mode", "batch", "-nojournal",
                                     "-log", "vivado.log", "-source", "run.tcl"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Copy the environment, optionally pointing Vivado at a license server.
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        const std::string var = *e;
        if (!opts.license.empty() &&
            (starts_with(var, "XILINXD_LICENSE_FILE=") || starts_with(var, "LM_LICENSE_FILE="))) {
            continue;
        }
        env.push_back(var);
    }
    if (!opts.license.empty()) {
        env.push_back("XILINXD_LICENSE_FILE=" + opts.license);
        env.push_back("LM_LICENSE_FILE=" + opts.license);
    }
    std::vector<char*> envp;
    for (auto& v : env) envp.push_back(v.data());
    envp.push_back(nullptr);

    const std::string dir = run_dir.string();
    const std::string out_file = (run_dir / "stdout.txt").string();

    const pid_t pid = fork();
    if (pid == 0) {
        // ----- child process -----
        setpgid(0, 0);                                     // own process group (for Ctrl+C cleanup)
        if (chdir(dir.c_str()) != 0) _exit(127);
        const int fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) _exit(127);
        dup2(fd, STDOUT_FILENO);                           // Vivado's screen output -> stdout.txt
        dup2(fd, STDERR_FILENO);
        close(fd);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        execve(argv[0], argv.data(), envp.data());         // becomes Vivado; only returns on error
        _exit(127);
    }
    if (pid < 0) return -1;

    // ----- parent process -----
    remember_proc(pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    forget_proc(pid);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Run a shell command and capture what it prints. ok=false if it failed.
std::string capture(const std::string& command, bool& ok) {
    ok = false;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    ok = pclose(pipe) == 0;
    return trim(out);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool is_executable_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

std::string tcl_brace(const std::string& s) { return "{" + s + "}"; }

// ---------------------------------------------------------------- job files

void write_xdc(const fs::path& path, const std::string& clock, double period, bool io_false_path) {
    std::ofstream out(path);
    out << "# generated by vivado_ppa (C++)\n";
    out << "create_clock -period " << fixed(period, 3) << " -name " << clock
        << " [get_ports " << tcl_brace(clock) << "]\n";
    if (io_false_path) {
        out << "# only time the internal register-to-register paths\n";
        out << "set_false_path -from [get_ports -quiet -filter {DIRECTION == IN && NAME != "
            << clock << "}]\n";
        out << "set_false_path -to   [all_outputs]\n";
    }
}

void write_tcl(const fs::path& path, const Job& job, const Options& opts) {
    const Design& d = *job.design;

    std::string sources;
    for (const auto& s : d.sources) {
        if (!sources.empty()) sources += " ";
        sources += tcl_brace("src/" + s.filename().string());
    }

    std::string synth = "synth_design -top " + tcl_brace(d.top) + " -part " + tcl_brace(opts.part) +
                        " -mode out_of_context -directive " + opts.synth_directive;
    for (const auto& [key, value] : job.variant->generics) synth += " -generic " + key + "=" + value;

    std::ofstream out(path);
    out << "# generated by vivado_ppa (C++)\n"
        << "proc ppa_put {k v} { set fh [open metrics.txt a]; puts $fh \"$k $v\"; close $fh }\n"
        << "set fh [open metrics.txt w]; close $fh\n"
        << "ppa_put vivado [version -short]\n"
        << "create_project -in_memory -part " << tcl_brace(opts.part) << "\n"
        << "read_verilog -sv [list " << sources << "]\n"
        << "read_xdc constraints.xdc\n"
        << synth << "\n"
        << "ppa_put stage synth\n";

    if (opts.impl) {
        out << "opt_design -directive " << opts.impl_directive << "\n"
            << "place_design -directive " << opts.impl_directive << "\n"
            << "route_design -directive " << opts.impl_directive << "\n"
            << "ppa_put stage impl\n";
    }

    // Collect timing and area into metrics.txt and save the full reports.
    out << R"TCL(set p [get_timing_paths -quiet -setup -max_paths 1 -nworst 1]
if {$p ne ""} {
    ppa_put wns [get_property SLACK $p]
    ppa_put logic_levels [get_property LOGIC_LEVELS $p]
    ppa_put logic_delay [get_property DATAPATH_LOGIC_DELAY $p]
    ppa_put net_delay [get_property DATAPATH_NET_DELAY $p]
    ppa_put crit_start [get_property STARTPOINT_PIN $p]
    ppa_put crit_end [get_property ENDPOINT_PIN $p]
}
set h [get_timing_paths -quiet -hold -max_paths 1 -nworst 1]
if {$h ne ""} { ppa_put whs [get_property SLACK $h] }
ppa_put carry [llength [get_cells -quiet -hierarchical -filter {PRIMITIVE_TYPE =~ CLB.CARRY.*}]]
# backup area counts straight from the netlist (used if the report wording isn't recognized)
ppa_put cnt_luts [llength [get_cells -quiet -hierarchical -filter {PRIMITIVE_GROUP == LUT}]]
ppa_put cnt_ffs [llength [get_cells -quiet -hierarchical -filter {PRIMITIVE_GROUP == FLOP_LATCH}]]
ppa_put cnt_dsps [llength [get_cells -quiet -hierarchical -filter {REF_NAME =~ DSP48* || REF_NAME =~ DSP58* || REF_NAME == DSPCPLX || REF_NAME == DSPFP32}]]
report_utilization -file util.rpt
report_utilization -hierarchical -file util_hier.rpt
report_timing_summary -max_paths 10 -file timing_summary.rpt
)TCL";

    if (opts.checkpoint) out << "write_checkpoint -force design.dcp\n";
    out << "ppa_put status OK\n";
}

// Value from a map, or fallback if the key is missing.
std::string get_or(const std::map<std::string, std::string>& m, const std::string& key,
                   const std::string& fallback) {
    auto it = m.find(key);
    return it == m.end() ? fallback : it->second;
}

// Format an optional number, or "NA" if there isn't one.
std::string fmt_or_na(const std::optional<double>& v, int decimals) {
    return v ? fixed(*v, decimals) : kNA;
}

bool nonzero(const std::optional<double>& v) { return v && *v != 0.0; }

}  // namespace

// ---------------------------------------------------------------- public helpers

void say(const std::string& line) {
    static std::mutex print_mutex;
    std::lock_guard<std::mutex> lock(print_mutex);
    std::cout << line << std::endl;   // endl flushes, so progress shows up immediately
}

std::string find_vivado(const std::string& name_or_path) {
    std::string p = name_or_path;
    if (starts_with(p, "~")) {
        if (const char* home = std::getenv("HOME")) p = home + p.substr(1);
    }
    if (p.find('/') != std::string::npos) {
        return is_executable_file(p) ? fs::absolute(p).string() : "";
    }
    // A bare name like "vivado": search the folders listed in PATH, like the shell does.
    if (const char* path_env = std::getenv("PATH")) {
        for (const auto& dir : split_char(path_env, ':')) {
            const fs::path candidate = fs::path(dir.empty() ? "." : dir) / p;
            if (is_executable_file(candidate)) return fs::absolute(candidate).string();
        }
    }
    return "";
}

std::string git_commit(const fs::path& folder) {
    bool ok = false;
    const std::string q = shell_quote(folder.string());
    std::string commit = capture("git -C " + q + " rev-parse --short HEAD 2>/dev/null", ok);
    if (!ok || commit.empty()) return "nogit";
    const std::string dirty = capture("git -C " + q + " status --porcelain -- . 2>/dev/null", ok);
    if (ok && !dirty.empty()) commit += "-dirty";
    return commit;
}

void install_interrupt_handler() {
    struct sigaction sa {};
    sa.sa_handler = on_interrupt;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
}

// ---------------------------------------------------------------- one job

Row run_job(const Job& job, const Options& opts, int index, int total,
            const fs::path& root, ResultsCsv& csv) {
    const Design& d = *job.design;
    const Variant& v = *job.variant;
    const double period = job.period;
    const std::string tag = "[" + std::to_string(index) + "/" + std::to_string(total) + "] " +
                            d.name + " " + v.label + " @ " + general(period) + " ns";

    // 1. Make a fresh folder for this run: runs/<date_time>_<design>_<variant>_p<period>
    const fs::path runs = root / "runs";
    fs::create_directories(runs);
    const std::string base = now_str("%Y%m%d_%H%M%S") + "_" + d.name + "_" + v.label + "_p" + general(period);
    fs::path run_dir = runs / base;
    for (int n = 2; !fs::create_directory(run_dir); ++n) {     // false = name already taken
        run_dir = runs / (base + "_" + std::to_string(n));
    }

    // 2. Snapshot the design's files, then write the clock constraint and Tcl script.
    fs::create_directory(run_dir / "src");
    for (const auto& entry : fs::directory_iterator(d.dir)) {
        if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), run_dir / "src" / entry.path().filename(),
                          fs::copy_options::overwrite_existing);
        }
    }
    write_xdc(run_dir / "constraints.xdc", d.clock, period, d.io_false_path);
    write_tcl(run_dir / "run.tcl", job, opts);

    // 3. Run Vivado.
    say(tag + "  started");
    const auto t0 = std::chrono::steady_clock::now();
    const int exit_code = opts.dry_run ? 0 : run_vivado(run_dir, opts);
    const double runtime = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // 4. Read what it produced.
    const auto m = read_metrics(run_dir / "metrics.txt");
    auto u = parse_util(run_dir / "util.rpt");
    for (const std::string k : {"luts", "ffs", "dsps"}) {
        const std::string backup = get_or(m, "cnt_" + k, "");
        if (u[k] == kNA && is_digits(backup)) u[k] = backup;
    }
    std::map<std::string, std::string> gd(v.generics.begin(), v.generics.end());

    std::string status, error;
    if (opts.dry_run) {
        status = "DRY_RUN";
    } else if (get_or(m, "status", "") == "OK") {
        status = "OK";
    } else {
        status = "FAILED";
        error = log_errors(run_dir);
        std::ofstream(run_dir / "error.txt") << "exit code " << exit_code << "\n" << error << "\n";
    }

    // 5. Derived numbers. Fmax comes from how much slack was left at the requested period.
    const auto wns = to_number(get_or(m, "wns", ""));
    std::optional<double> fmax;
    if (wns && period - *wns > 0) fmax = 1000.0 / (period - *wns);
    const auto elems = to_number(!d.elements.empty() ? d.elements : get_or(gd, "N", ""));
    const double ii = nonzero(to_number(d.ii)) ? *to_number(d.ii) : 1.0;
    const auto luts = to_number(u["luts"]);

    std::optional<double> gelem, luts_per_elem, efficiency;
    if (nonzero(fmax) && nonzero(elems)) gelem = *fmax * *elems / ii / 1000.0;
    if (nonzero(luts) && nonzero(elems)) luts_per_elem = *luts / *elems;
    if (nonzero(gelem) && nonzero(luts)) efficiency = *gelem * 1000.0 / *luts;

    std::string generics;
    for (const auto& [key, value] : v.generics) {
        if (!generics.empty()) generics += " ";
        generics += key + "=" + value;
    }

    // 6. Assemble the row (same columns as the Python version).
    Row row;
    row["date"] = now_str("%Y-%m-%d %H:%M");
    row["design"] = d.name;
    row["variant"] = v.label;
    row["EXP"] = get_or(gd, "EXP", "");
    row["MAN"] = get_or(gd, "MAN", "");
    row["N"] = get_or(gd, "N", "");
    row["STAGES"] = get_or(gd, "STAGES", "");
    row["generics"] = generics;
    row["period_ns"] = general(period);
    row["target_mhz"] = fixed(1000.0 / period, 1);
    row["stage"] = get_or(m, "stage", kNA);
    row["status"] = status;
    row["wns_ns"] = get_or(m, "wns", kNA);
    row["whs_ns"] = get_or(m, "whs", kNA);
    row["fmax_mhz"] = fmt_or_na(fmax, 1);
    row["timing_met"] = !wns ? kNA : (*wns >= 0 ? "yes" : "no");
    row["luts"] = u["luts"];
    row["ffs"] = u["ffs"];
    row["dsps"] = u["dsps"];
    row["bram"] = u["bram"];
    row["carry"] = get_or(m, "carry", kNA);
    row["latency_cycles"] = v.latency;
    row["ii"] = general(ii);
    row["elements"] = elems ? general(*elems) : kNA;
    row["gelem_s"] = fmt_or_na(gelem, 2);
    row["luts_per_elem"] = fmt_or_na(luts_per_elem, 1);
    row["melem_s_per_lut"] = fmt_or_na(efficiency, 3);
    row["logic_levels"] = get_or(m, "logic_levels", kNA);
    row["logic_delay_ns"] = get_or(m, "logic_delay", kNA);
    row["net_delay_ns"] = get_or(m, "net_delay", kNA);
    row["crit_start"] = get_or(m, "crit_start", kNA);
    row["crit_end"] = get_or(m, "crit_end", kNA);
    row["part"] = opts.part;
    row["vivado"] = get_or(m, "vivado", kNA);
    row["synth_directive"] = opts.synth_directive;
    row["impl_directive"] = opts.impl ? opts.impl_directive : "";
    row["runtime_s"] = fixed(runtime, 0);
    row["commit"] = opts.commit;
    row["run_dir"] = fs::relative(run_dir, root).string();
    row["note"] = opts.note;
    row["error"] = error;

    if (!opts.dry_run) csv.append(row);

    // 7. Report progress.
    if (status == "OK") {
        say(tag + "  done in " + fixed(runtime, 0) + "s: Fmax " + row["fmax_mhz"] + " MHz, WNS " +
            row["wns_ns"] + " ns, LUTs " + row["luts"] + ", FFs " + row["ffs"] + ", DSPs " + row["dsps"]);
    } else if (status == "FAILED") {
        say(tag + "  FAILED after " + fixed(runtime, 0) + "s: " + error);
        const bool have_log = fs::exists(run_dir / "vivado.log");
        say("      details: " + (fs::path(row["run_dir"]) / (have_log ? "vivado.log" : "stdout.txt")).string());
    } else {
        say(tag + "  dry run, files written to " + row["run_dir"]);
    }
    return row;
}

}  // namespace ppa
