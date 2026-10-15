#!/usr/bin/env python3
"""
vivado_ppa.py

Runs Vivado in batch mode (no GUI) on the designs you choose, at the clock
period(s) you choose, and appends timing + area results to results.csv.

  python3 vivado_ppa.py                       interactive: asks what to run
  python3 vivado_ppa.py --list                show designs and their variants
  python3 vivado_ppa.py mxfp6_comb --period 5 --note "baseline"
  python3 vivado_ppa.py mxfp6_comb mx_param --period 4 5 6
  python3 vivado_ppa.py mx_param --variants E2M3 E4M3 --period 5 --synth-only
  python3 vivado_ppa.py mx_param --period 5 --jobs 3     (3 Vivado runs at once)

Folder layout (next to this script):
  settings.ini              Vivado path, FPGA part, default options
  designs/<name>/design.ini one folder per design: its .sv files + design.ini
  results.csv               created/extended automatically
  runs/<...>/               one folder per run: source snapshot, reports, log

Works with Python 3.6+. No extra packages needed.
"""

import argparse
import configparser
import csv
import datetime
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.abspath(__file__))
DESIGNS_DIR = os.path.join(ROOT, "designs")
RUNS_DIR = os.path.join(ROOT, "runs")
RESULTS_CSV = os.path.join(ROOT, "results.csv")
SETTINGS_INI = os.path.join(ROOT, "settings.ini")

COLUMNS = [
    "date", "design", "variant", "EXP", "MAN", "N", "STAGES", "generics",
    "period_ns", "target_mhz", "stage", "status",
    "wns_ns", "whs_ns", "fmax_mhz", "timing_met",
    "luts", "ffs", "dsps", "bram", "carry",
    "latency_cycles", "ii", "elements", "gelem_s", "luts_per_elem", "melem_s_per_lut",
    "logic_levels", "logic_delay_ns", "net_delay_ns", "crit_start", "crit_end",
    "part", "vivado", "synth_directive", "impl_directive", "runtime_s",
    "commit", "run_dir", "note", "error",
]

NA = "NA"
_print_lock = threading.Lock()
_csv_lock = threading.Lock()
_procs = []


def say(msg=""):
    with _print_lock:
        print(msg, flush=True)


def die(msg):
    print("\nERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def make_parser():
    # inline # comments allowed; keys keep their case (variant labels like E2M3)
    p = configparser.ConfigParser(inline_comment_prefixes=("#", ";"))
    p.optionxform = str
    return p


# ---------------------------------------------------------------- settings

def load_settings():
    s = {
        "vivado": "vivado",
        "part": "",
        "period_ns": "10",
        "implementation": "yes",
        "synth_directive": "default",
        "impl_directive": "Default",
        "jobs": "1",
        "save_checkpoint": "no",
    }
    if os.path.exists(SETTINGS_INI):
        p = make_parser()
        p.read(SETTINGS_INI)
        for sec in p.sections():
            for k, v in p.items(sec):
                if k == "path":
                    k = "vivado"
                s[k] = v.strip()
    return s


def yes(v):
    return str(v).strip().lower() in ("1", "yes", "y", "true", "on")


def find_vivado(path):
    if os.path.sep in path or path.startswith("~"):
        path = os.path.expanduser(path)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
        return None
    return shutil.which(path)


# ---------------------------------------------------------------- designs

def list_designs():
    if not os.path.isdir(DESIGNS_DIR):
        return []
    out = []
    for name in sorted(os.listdir(DESIGNS_DIR)):
        if name.startswith("_") or name.startswith("."):
            continue
        if os.path.isfile(os.path.join(DESIGNS_DIR, name, "design.ini")):
            out.append(name)
    return out


def load_design(name):
    ddir = os.path.join(DESIGNS_DIR, name)
    ini = os.path.join(ddir, "design.ini")
    if not os.path.isfile(ini):
        have = ", ".join(list_designs()) or "(none)"
        die("No design called '{}' (looked for {}).\nAvailable designs: {}".format(name, ini, have))

    p = make_parser()
    p.read(ini)
    if not p.has_section("design") or not p.has_section("variants"):
        die("{} needs a [design] section and a [variants] section.".format(ini))
    d = p["design"]

    for key in ("top", "sources"):
        if not d.get(key, "").strip():
            die("{}: '{}' is missing under [design].".format(ini, key))

    sources = []
    for f in d["sources"].split():
        path = os.path.join(ddir, f)
        if not os.path.isfile(path):
            die("{}: source file '{}' not found in {}".format(ini, f, ddir))
        sources.append(path)

    latency = d.get("latency", "2").strip()
    variants = []
    for label, spec in p.items("variants"):
        generics, lat = [], latency
        for tok in spec.split():
            if "=" not in tok:
                die("{}: variant '{}' has '{}'; expected PARAM=value".format(ini, label, tok))
            k, v = tok.split("=", 1)
            if k.lower() == "latency":
                lat = v
            else:
                generics.append((k, v))
        variants.append({"label": label, "generics": generics, "latency": lat})
    if not variants:
        die("{}: [variants] is empty.".format(ini))

    return {
        "name": name,
        "dir": ddir,
        "top": d["top"].strip(),
        "sources": sources,
        "clock": d.get("clock", "clk").strip(),
        "ii": d.get("ii", "1").strip(),
        "elements": d.get("elements", "").strip(),
        "io_false_path": yes(d.get("io_false_path", "yes")),
        "variants": variants,
    }


# ---------------------------------------------------------------- Vivado job files

def tcl_brace(s):
    return "{" + s + "}"


def write_xdc(path, clock, period, io_false_path):
    lines = [
        "# generated by vivado_ppa.py",
        "create_clock -period {:.3f} -name {} [get_ports {}]".format(period, clock, tcl_brace(clock)),
    ]
    if io_false_path:
        lines += [
            "# only time the internal register-to-register paths",
            "set_false_path -from [get_ports -quiet -filter {{DIRECTION == IN && NAME != {}}}]".format(clock),
            "set_false_path -to   [all_outputs]",
        ]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_tcl(path, job, opts):
    d = job["design"]
    gen = " ".join("-generic {}={}".format(k, v) for k, v in job["variant"]["generics"])
    srcs = " ".join(tcl_brace(os.path.join("src", os.path.basename(s))) for s in d["sources"])
    t = []
    t.append("# generated by vivado_ppa.py")
    t.append("proc ppa_put {k v} { set fh [open metrics.txt a]; puts $fh \"$k $v\"; close $fh }")
    t.append("set fh [open metrics.txt w]; close $fh")
    t.append("ppa_put vivado [version -short]")
    t.append("create_project -in_memory -part {}".format(tcl_brace(opts["part"])))
    t.append("read_verilog -sv [list {}]".format(srcs))
    t.append("read_xdc constraints.xdc")
    t.append("synth_design -top {} -part {} -mode out_of_context -directive {} {}".format(
        tcl_brace(d["top"]), tcl_brace(opts["part"]), opts["synth_directive"], gen).rstrip())
    t.append("ppa_put stage synth")
    if opts["impl"]:
        for cmd in ("opt_design", "place_design", "route_design"):
            t.append("{} -directive {}".format(cmd, opts["impl_directive"]))
        t.append("ppa_put stage impl")
    t += [
        "set p [get_timing_paths -quiet -setup -max_paths 1 -nworst 1]",
        "if {$p ne \"\"} {",
        "    ppa_put wns [get_property SLACK $p]",
        "    ppa_put logic_levels [get_property LOGIC_LEVELS $p]",
        "    ppa_put logic_delay [get_property DATAPATH_LOGIC_DELAY $p]",
        "    ppa_put net_delay [get_property DATAPATH_NET_DELAY $p]",
        "    ppa_put crit_start [get_property STARTPOINT_PIN $p]",
        "    ppa_put crit_end [get_property ENDPOINT_PIN $p]",
        "}",
        "set h [get_timing_paths -quiet -hold -max_paths 1 -nworst 1]",
        "if {$h ne \"\"} { ppa_put whs [get_property SLACK $h] }",
        "ppa_put carry [llength [get_cells -quiet -hierarchical -filter {PRIMITIVE_TYPE =~ CLB.CARRY.*}]]",
        "report_utilization -file util.rpt",
        "report_utilization -hierarchical -file util_hier.rpt",
        "report_timing_summary -max_paths 10 -file timing_summary.rpt",
    ]
    if opts["checkpoint"]:
        t.append("write_checkpoint -force design.dcp")
    t.append("ppa_put status OK")
    with open(path, "w") as f:
        f.write("\n".join(t) + "\n")


# ---------------------------------------------------------------- result parsing

def read_metrics(path):
    m = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.rstrip("\n")
                if line:
                    k, _, v = line.partition(" ")
                    m[k] = v.strip()
    return m


def parse_util(path):
    u = {"luts": NA, "ffs": NA, "dsps": NA, "bram": NA}
    if not os.path.exists(path):
        return u
    with open(path) as f:
        txt = f.read()
    pats = {
        "luts": r"^\|\s*(?:Slice|CLB) LUTs\*?\s*\|\s*(\d+)",
        "ffs": r"^\|\s*(?:Slice|CLB) Registers\*?\s*\|\s*(\d+)",
        "dsps": r"^\|\s*DSPs\*?\s*\|\s*(\d+)",
        "bram": r"^\|\s*Block RAM Tile\*?\s*\|\s*([\d.]+)",
    }
    for k, pat in pats.items():
        mm = re.search(pat, txt, re.M)
        if mm:
            u[k] = mm.group(1)
    return u


def num(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def log_errors(log_path, limit=3):
    if not os.path.exists(log_path):
        return "Vivado did not start (no vivado.log)"
    errs = []
    with open(log_path, errors="replace") as f:
        for line in f:
            if line.startswith("ERROR:") or line.startswith("CRITICAL WARNING:"):
                errs.append(line.strip())
    errs = [e for e in errs if e.startswith("ERROR:")] or errs
    return " | ".join(errs[:limit]) if errs else "unknown failure, see vivado.log"


def git_commit(path):
    try:
        out = subprocess.check_output(["git", "-C", path, "rev-parse", "--short", "HEAD"],
                                      stderr=subprocess.DEVNULL, universal_newlines=True).strip()
        dirty = subprocess.check_output(["git", "-C", path, "status", "--porcelain", "--", "."],
                                        stderr=subprocess.DEVNULL, universal_newlines=True).strip()
        return out + ("-dirty" if dirty else "")
    except Exception:
        return "nogit"


# ---------------------------------------------------------------- CSV

def ensure_csv():
    if os.path.exists(RESULTS_CSV):
        with open(RESULTS_CSV, newline="") as f:
            first = next(csv.reader(f), [])
        if first == COLUMNS:
            return
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        old = os.path.join(ROOT, "results_old_{}.csv".format(stamp))
        os.rename(RESULTS_CSV, old)
        say("NOTE: results.csv had different columns; moved it to {}".format(os.path.basename(old)))
    with open(RESULTS_CSV, "w", newline="") as f:
        csv.writer(f).writerow(COLUMNS)


def append_row(row):
    with _csv_lock:
        with open(RESULTS_CSV, "a", newline="") as f:
            csv.DictWriter(f, fieldnames=COLUMNS).writerow(row)


# ---------------------------------------------------------------- one job

def run_job(job, opts, idx, total):
    d, v, period = job["design"], job["variant"], job["period"]
    tag = "[{}/{}] {} {} @ {:g} ns".format(idx, total, d["name"], v["label"], period)

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    rdir = os.path.join(RUNS_DIR, "{}_{}_{}_p{:g}".format(stamp, d["name"], v["label"], period))
    n = 1
    while os.path.exists(rdir):
        n += 1
        rdir = os.path.join(RUNS_DIR, "{}_{}_{}_p{:g}_{}".format(stamp, d["name"], v["label"], period, n))
    src = os.path.join(rdir, "src")
    os.makedirs(src)
    for f in os.listdir(d["dir"]):
        fp = os.path.join(d["dir"], f)
        if os.path.isfile(fp):
            shutil.copy2(fp, src)

    write_xdc(os.path.join(rdir, "constraints.xdc"), d["clock"], period, d["io_false_path"])
    write_tcl(os.path.join(rdir, "run.tcl"), job, opts)

    say("{}  started".format(tag))
    t0 = time.time()
    if opts["dry_run"]:
        rc = 0
    else:
        with open(os.path.join(rdir, "stdout.txt"), "w") as out:
            proc = subprocess.Popen(
                [opts["vivado"], "-mode", "batch", "-nojournal",
                 "-log", "vivado.log", "-source", "run.tcl"],
                cwd=rdir, stdout=out, stderr=subprocess.STDOUT)
            _procs.append(proc)
            rc = proc.wait()
    runtime = time.time() - t0

    m = read_metrics(os.path.join(rdir, "metrics.txt"))
    u = parse_util(os.path.join(rdir, "util.rpt"))
    gd = dict(v["generics"])

    if opts["dry_run"]:
        status, error = "DRY_RUN", ""
    elif m.get("status") == "OK":
        status, error = "OK", ""
    else:
        status, error = "FAILED", log_errors(os.path.join(rdir, "vivado.log"))
        with open(os.path.join(rdir, "error.txt"), "w") as f:
            f.write("exit code {}\n{}\n".format(rc, error))

    wns = num(m.get("wns"))
    fmax = 1000.0 / (period - wns) if wns is not None and period - wns > 0 else None
    elems = num(d["elements"] or gd.get("N"))
    ii = num(d["ii"]) or 1.0
    luts = num(u["luts"])
    gelem = fmax * elems / ii / 1000.0 if fmax and elems else None

    def f(x, fmt):
        return fmt.format(x) if x is not None else NA

    row = {
        "date": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "design": d["name"], "variant": v["label"],
        "EXP": gd.get("EXP", ""), "MAN": gd.get("MAN", ""),
        "N": gd.get("N", ""), "STAGES": gd.get("STAGES", ""),
        "generics": " ".join("{}={}".format(k, x) for k, x in v["generics"]),
        "period_ns": "{:g}".format(period), "target_mhz": "{:.1f}".format(1000.0 / period),
        "stage": m.get("stage", NA), "status": status,
        "wns_ns": m.get("wns", NA), "whs_ns": m.get("whs", NA),
        "fmax_mhz": f(fmax, "{:.1f}"),
        "timing_met": NA if wns is None else ("yes" if wns >= 0 else "no"),
        "luts": u["luts"], "ffs": u["ffs"], "dsps": u["dsps"], "bram": u["bram"],
        "carry": m.get("carry", NA),
        "latency_cycles": v["latency"], "ii": "{:g}".format(ii),
        "elements": f(elems, "{:g}"),
        "gelem_s": f(gelem, "{:.2f}"),
        "luts_per_elem": f(luts / elems if luts and elems else None, "{:.1f}"),
        "melem_s_per_lut": f(gelem * 1000.0 / luts if gelem and luts else None, "{:.3f}"),
        "logic_levels": m.get("logic_levels", NA),
        "logic_delay_ns": m.get("logic_delay", NA), "net_delay_ns": m.get("net_delay", NA),
        "crit_start": m.get("crit_start", NA), "crit_end": m.get("crit_end", NA),
        "part": opts["part"], "vivado": m.get("vivado", NA),
        "synth_directive": opts["synth_directive"],
        "impl_directive": opts["impl_directive"] if opts["impl"] else "",
        "runtime_s": "{:.0f}".format(runtime),
        "commit": opts["commit"], "run_dir": os.path.relpath(rdir, ROOT),
        "note": opts["note"], "error": error,
    }
    if not opts["dry_run"]:
        append_row(row)

    if status == "OK":
        say("{}  done in {:.0f}s: Fmax {} MHz, WNS {} ns, LUTs {}, FFs {}, DSPs {}".format(
            tag, runtime, row["fmax_mhz"], row["wns_ns"], row["luts"], row["ffs"], row["dsps"]))
    elif status == "FAILED":
        say("{}  FAILED after {:.0f}s: {}".format(tag, runtime, error))
        say("      details: {}".format(os.path.join(row["run_dir"], "vivado.log")))
    else:
        say("{}  dry run, files written to {}".format(tag, row["run_dir"]))
    return row


# ---------------------------------------------------------------- interactive mode

def ask(prompt, default=""):
    tail = " [{}]".format(default) if default != "" else ""
    try:
        ans = input("{}{}: ".format(prompt, tail)).strip()
    except EOFError:
        ans = ""
    return ans or default


def pick(items, prompt):
    for i, it in enumerate(items, 1):
        print("  {}. {}".format(i, it))
    while True:
        ans = ask(prompt + " (numbers or names separated by spaces, or 'all')", "all")
        if ans.lower() == "all":
            return list(items)
        chosen, ok = [], True
        for tok in ans.replace(",", " ").split():
            if tok.isdigit() and 1 <= int(tok) <= len(items):
                chosen.append(items[int(tok) - 1])
            elif tok in items:
                chosen.append(tok)
            else:
                print("  '{}' is not on the list, try again.".format(tok))
                ok = False
                break
        if ok and chosen:
            return list(dict.fromkeys(chosen))


def interactive(args, settings):
    designs = list_designs()
    if not designs:
        die("No designs found in {}. Each design needs its own folder with a design.ini.".format(DESIGNS_DIR))
    print("\nDesigns:")
    args.designs = pick(designs, "Which designs")

    all_labels = []
    for name in args.designs:
        for v in load_design(name)["variants"]:
            if v["label"] not in all_labels:
                all_labels.append(v["label"])
    if len(all_labels) > 1:
        print("\nVariants in the chosen designs:")
        chosen = pick(all_labels, "Which variants")
        args.variants = None if chosen == all_labels else chosen

    while True:
        ans = ask("\nClock period(s) in ns, e.g. 5 or 4 5 6", settings["period_ns"])
        try:
            args.period = [float(x) for x in ans.replace(",", " ").split()]
            if all(p > 0 for p in args.period):
                break
        except ValueError:
            pass
        print("  Enter positive numbers, like 5 or 4.5 5 6")

    impl_default = "i" if yes(settings["implementation"]) else "s"
    ans = ask("Synthesis only (s, fast) or full implementation (i, accurate timing)", impl_default)
    args.synth_only = ans.lower().startswith("s")
    args.note = ask("Note describing this run (what changed)", "")
    args.jobs = int(ask("How many Vivado runs at once", settings["jobs"]) or 1)


# ---------------------------------------------------------------- main

def main():
    settings = load_settings()

    ap = argparse.ArgumentParser(
        description="Run Vivado (batch, no GUI) on designs/<name>/ and log timing + area to results.csv. "
                    "Run with no arguments for interactive mode.")
    ap.add_argument("designs", nargs="*", help="design folder names in designs/ (or 'all')")
    ap.add_argument("-p", "--period", nargs="+", type=float, metavar="NS",
                    help="clock period(s) in ns; several values = one run each (default from settings.ini)")
    ap.add_argument("-v", "--variants", nargs="+", metavar="LABEL", help="only run these variant labels")
    ap.add_argument("-n", "--note", default="", help="note saved with every row")
    ap.add_argument("-s", "--synth-only", action="store_true", help="skip place and route (faster)")
    ap.add_argument("-i", "--impl", action="store_true", help="force full implementation")
    ap.add_argument("-j", "--jobs", type=int, help="Vivado runs at once (default from settings.ini)")
    ap.add_argument("--part", help="override the FPGA part from settings.ini")
    ap.add_argument("--vivado", help="path to the vivado program")
    ap.add_argument("--checkpoint", action="store_true", help="also save a .dcp checkpoint per run")
    ap.add_argument("--list", action="store_true", help="list designs and variants, then exit")
    ap.add_argument("--dry-run", action="store_true", help="write the Vivado files but don't run Vivado")
    args = ap.parse_args()

    if args.list:
        names = list_designs()
        if not names:
            print("No designs found in {}".format(DESIGNS_DIR))
        for name in names:
            d = load_design(name)
            print("\n{}   (top: {}, files: {})".format(
                name, d["top"], " ".join(os.path.basename(s) for s in d["sources"])))
            for v in d["variants"]:
                g = " ".join("{}={}".format(k, x) for k, x in v["generics"])
                print("    {:<12} {}".format(v["label"], g))
        return

    if not args.designs:
        interactive(args, settings)
    if args.designs == ["all"]:
        args.designs = list_designs()

    # options
    part = args.part or settings["part"]
    if not part:
        die("No FPGA part set. Put it in settings.ini (part = ...) or pass --part.")
    impl = yes(settings["implementation"])
    if args.synth_only:
        impl = False
    if args.impl:
        impl = True
    periods = args.period or [float(settings["period_ns"])]
    if any(p <= 0 for p in periods):
        die("Clock periods must be positive.")

    # build the job list
    jobs = []
    for name in args.designs:
        d = load_design(name)
        vs = d["variants"]
        if args.variants:
            vs = [v for v in vs if v["label"] in args.variants]
            if not vs:
                say("NOTE: {} has none of the variants {}; skipping it.".format(name, " ".join(args.variants)))
        for v in vs:
            for p in periods:
                jobs.append({"design": d, "variant": v, "period": p})
    if not jobs:
        die("Nothing to run. Check the design and variant names with:  python3 vivado_ppa.py --list")

    vivado = find_vivado(args.vivado or settings["vivado"])
    if not vivado and not args.dry_run:
        die("Can't find the vivado program ('{}').\n"
            "  Fix: open settings.ini and set 'path' to the full path of vivado.\n"
            "  To find it, open the Vivado GUI once and type in its Tcl Console:\n"
            "      puts $env(XILINX_VIVADO)/bin/vivado\n"
            "  and paste what it prints into settings.ini.".format(args.vivado or settings["vivado"]))

    opts = {
        "vivado": vivado, "part": part, "impl": impl,
        "synth_directive": settings["synth_directive"], "impl_directive": settings["impl_directive"],
        "checkpoint": args.checkpoint or yes(settings["save_checkpoint"]),
        "note": args.note, "dry_run": args.dry_run, "commit": git_commit(ROOT),
    }
    nj = max(1, args.jobs or int(settings["jobs"]))

    say("\nRunning {} job(s): {}, part {}, {} at a time".format(
        len(jobs), "synth + place + route" if impl else "synthesis only", part, nj))
    say("Clock period(s): {} ns".format(", ".join("{:g}".format(p) for p in periods)))
    say("Results go to {}\n".format(RESULTS_CSV))

    os.makedirs(RUNS_DIR, exist_ok=True)
    if not args.dry_run:
        ensure_csv()

    rows, t0 = [], time.time()
    try:
        with ThreadPoolExecutor(max_workers=nj) as pool:
            futs = [pool.submit(run_job, j, opts, i + 1, len(jobs)) for i, j in enumerate(jobs)]
            for fu in as_completed(futs):
                rows.append(fu.result())
    except KeyboardInterrupt:
        say("\nStopping: killing running Vivado jobs...")
        for p in _procs:
            if p.poll() is None:
                p.terminate()
        sys.exit(130)

    # summary
    rows.sort(key=lambda r: (r["design"], r["variant"], float(r["period_ns"])))
    say("\n" + "=" * 92)
    say("{:<14} {:<10} {:>7} {:>8} {:>9} {:>7} {:>7} {:>6}  {}".format(
        "design", "variant", "period", "WNS", "Fmax", "LUTs", "FFs", "DSPs", "status"))
    for r in rows:
        say("{:<14} {:<10} {:>7} {:>8} {:>9} {:>7} {:>7} {:>6}  {}".format(
            r["design"][:14], r["variant"][:10], r["period_ns"], r["wns_ns"], r["fmax_mhz"],
            r["luts"], r["ffs"], r["dsps"], r["status"]))
    say("=" * 92)
    nfail = sum(r["status"] == "FAILED" for r in rows)
    say("{} run(s) in {:.0f}s, {} failed. Results: {}".format(len(rows), time.time() - t0, nfail, RESULTS_CSV))
    if nfail:
        sys.exit(1)


if __name__ == "__main__":
    main()
