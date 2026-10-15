# ppa_settings.tcl
# Settings shared by every design. Set these once and leave them alone,
# otherwise new results can't be compared with old ones.

# FPGA part. Get it from the Vivado Tcl console with:  get_property PART [current_project]
set part xcv80-lsva4737-2MHP-e-S

# 1 = synthesis + place + route (accurate timing, slower)
# 0 = synthesis only (fast, timing is an estimate)
set run_impl 1

# Tool strategies. "default" / "Default" are Vivado's normal settings.
# impl_directive is used for opt_design, place_design and route_design;
# safe values are Default or Explore.
set synth_directive default
set impl_directive  Default
