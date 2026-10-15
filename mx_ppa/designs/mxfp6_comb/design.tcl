# Your E2M3 FP6 multiplier (mxfp6_mult), with registered inputs/outputs (mxfp6_wrapper)

set top  mxfp6_wrapper
set srcs {mxfp6_mult.sv mxfp6_wrapper.sv}

# cycles from input port to output port: input reg + output reg, no internal stages
set latency 2

# a new block can enter every cycle
set ii 1

# {label {PARAM=value ...}}
set variants {
    {E2M3    {N=32}}
    {E2M3_N2 {N=2}}
}
