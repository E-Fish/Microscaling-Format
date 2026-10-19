vivado_ppa, C++ version
=======================

Same tool as vivado_ppa.py: same options, same settings.ini and designs/ folders,
same results.csv columns. Rows from either version go into the same results.csv.

BUILD (once, and again after changing any .cpp/.hpp file)
    cd vivado_ppa/cpp
    make
  This creates the program  vivado_ppa/vivado_ppa  (next to settings.ini).
  With an old compiler (g++ 8) use:  make LDLIBS=-lstdc++fs

RUN (from the vivado_ppa folder)
    ./vivado_ppa --list
    ./vivado_ppa mx_param --period 2 --note "all formats, 2 ns"
    ./vivado_ppa --help
  Every option works the same as  python3 vivado_ppa.py ...

FILES
  src/main.cpp      command line, interactive mode, worker threads, summary table
  src/config.*      reading settings.ini and designs/<name>/design.ini (IniFile class)
  src/runner.*      one Vivado run: writes constraints.xdc + run.tcl, starts Vivado
                    (fork/exec), turns its output into a results row
  src/results.*     parsing metrics.txt / util.rpt / vivado.log, writing results.csv
                    (thread-safe ResultsCsv class)
  src/util.hpp      small string / number / time helpers
  Makefile          compiles each .cpp into build/*.o, then links ../vivado_ppa

  make clean        removes build/ and the program
