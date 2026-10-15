vivado_ppa: run Vivado without opening it, collect timing + area into results.csv
==================================================================================

ONE-TIME SETUP
  1. Unzip this folder on the lab server.
  2. Open settings.ini and set:
       part = your FPGA part
              (Vivado GUI Tcl Console:  get_property PART [current_project])
       path = the vivado program, only if typing  vivado -version  in a terminal fails
              (Vivado GUI Tcl Console:  puts $env(XILINX_VIVADO)/bin/vivado)
  3. In a terminal:  cd <this folder>   then   python3 vivado_ppa.py --list
     You should see your designs listed.

EVERY RUN
  Easiest (asks you questions):
      python3 vivado_ppa.py

  Or all in one line:
      python3 vivado_ppa.py mxfp6_comb --period 5 --note "baseline"
      python3 vivado_ppa.py mxfp6_comb mx_param --period 4 5 6
      python3 vivado_ppa.py mx_param --variants E2M3 E4M3 --period 5 --synth-only
      python3 vivado_ppa.py all --period 5 --jobs 2

  Options:
      --period 4 5 6     clock period(s) in ns; one run per period
      --variants A B     only these variant labels
      --note "..."       what changed (saved in every row)
      --synth-only       skip place and route (faster, rougher timing)
      --jobs 2           run 2 Vivado jobs at once
      --checkpoint       also save a .dcp you can open in Vivado later
      --list             show designs and variants
      --help             show all options

  Stop a run with Ctrl+C.

RESULTS
  results.csv          one row per run, never overwritten (open in Excel / Google Sheets)
  runs/<date>_<design>_<variant>_p<period>/
                       src/ (exact copy of the code used), constraints.xdc, run.tcl,
                       util.rpt, util_hier.rpt, timing_summary.rpt, vivado.log

ADDING A DESIGN
  1. cp -r designs/_template designs/<new_name>
  2. Put your .sv files in designs/<new_name>/
  3. Edit designs/<new_name>/design.ini (top, sources, clock, latency, variants)
  4. python3 vivado_ppa.py <new_name> --period 5

KEY COLUMNS
  fmax_mhz         max clock speed reached        timing_met   yes if WNS >= 0
  luts ffs dsps    area                           latency_cycles  cycles per result
  gelem_s          throughput (billion elements per second)
  melem_s_per_lut  throughput per LUT (higher = more efficient); fairest way to
                   compare pipelined vs combinational designs
