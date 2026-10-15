# Target clock: 10 ns (100 MHz). Tighten the period to find Fmax.
create_clock -period 3.000 -name clk [get_ports clk]

# Optional: keep I/O paths out of the report so only the internal
# reg -> mxfp6_mult -> reg path is analyzed.
set_false_path -from [get_ports {a[*] b[*]}]
set_false_path -to   [get_ports {c[*]}]
