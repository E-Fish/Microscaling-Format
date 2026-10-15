# Generic parameterized MX multiplier (mx_mult): all five MX formats from one design

set top  mx_wrapper
set srcs {mx_mult.sv mx_wrapper.sv}

# cycles from input port to output port: input reg + output reg, no internal stages
set latency 2

# a new block can enter every cycle
set ii 1

# {label {PARAM=value ...}}
set variants {
    {E2M1 {EXP=2 MAN=1 N=32}}
    {E2M3 {EXP=2 MAN=3 N=32}}
    {E3M2 {EXP=3 MAN=2 N=32}}
    {E4M3 {EXP=4 MAN=3 N=32}}
    {E5M2 {EXP=5 MAN=2 N=32}}
}
