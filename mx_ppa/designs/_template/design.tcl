# TEMPLATE: copy this whole folder to designs/<new_name>/, put your .sv files in it,
# and fill in the settings below.

# Top-level module to synthesize. It should register its inputs and outputs
# (like mx_wrapper) and use a clock port named clk.
set top  my_wrapper

# Your .sv files in this folder (file names only)
set srcs {my_core.sv my_wrapper.sv}

# Cycles from input port to output port.
#   combinational core + input/output registers = 2
#   core with 3 internal pipeline stages        = 2 + 3 = 5
set latency 2

# Cycles between new inputs. 1 = fully pipelined (a new block every cycle).
set ii 1

# Optional: elements processed per input. Leave commented out if your design
# has an N parameter; N is used automatically.
# set elements 32

# One line per variant: {label {PARAM=value PARAM=value ...} optional_latency}
# Labels: no spaces. PARAM names must match your top module's parameters.
# EXP, MAN, N and STAGES get their own columns in results.csv; any other
# parameter is still saved in the "generics" column.
# The third item overrides latency for that variant (handy when STAGES changes).
set variants {
    {E2M3    {EXP=2 MAN=3 N=32}}
    {E2M3_S3 {EXP=2 MAN=3 N=32 STAGES=3} 5}
}

# Optional: to use a different clock target for this design only, put a
# timing.xdc in this folder. Otherwise the shared mx_ppa/timing.xdc is used.
