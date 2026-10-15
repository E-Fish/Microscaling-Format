# run_ppa.tcl
# General timing + area runner for any design in designs/<name>/.
# You should never need to edit this file:
#   - lab-wide settings (part, strategy) live in ppa_settings.tcl
#   - each design's settings live in designs/<name>/design.tcl
#
# From the Vivado GUI Tcl console (close any open project first):
#   cd /path/to/mx_ppa
#   set design mx_comb; set note "baseline"; source run_ppa.tcl
#   (optional, run only some variants:  set only {E2M3 E4M3}  before source)
#
# From a shell:
#   vivado -mode batch -nojournal -source run_ppa.tcl -tclargs mx_comb "baseline"
#   vivado -mode batch -nojournal -source run_ppa.tcl -tclargs mx_comb "baseline" E2M3,E4M3
#
# Every run appends rows to results.csv and creates runs/<date_time>_<design>_<variant>/
# holding a copy of the source files, the reports, and a checkpoint.

cd [file dirname [file normalize [info script]]]

set PPA_HEADER {date design variant EXP MAN N STAGES generics latency_cycles ii elements
                part vivado synth_directive impl_directive stage period_ns wns_ns fmax_mhz
                gelem_s luts ffs dsps carry luts_per_elem melem_s_per_lut
                logic_levels logic_delay_ns net_delay_ns status commit run_dir note}

proc ppa_grab {rpt pattern} {
    if {[regexp $pattern $rpt -> v]} { return $v }
    return NA
}

proc ppa_csv {s} {
    return "\"[string map {\" \"\"} $s]\""
}

proc ppa_dget {d k} {
    if {[dict exists $d $k]} { return [dict get $d $k] }
    return ""
}

proc ppa_num {x} {
    return [string is double -strict $x]
}

proc ppa_run {design note only} {
    global PPA_HEADER

    # ---------- checks ----------
    if {[current_project -quiet] ne ""} {
        error "A project is open. Close it first (File > Close Project), then run again."
    }
    set ddir designs/$design
    if {![file isdirectory $ddir]} {
        set have [lsort [glob -nocomplain -tails -directory designs -type d *]]
        error "Folder designs/$design not found. Available designs: $have"
    }

    # ---------- settings ----------
    source ppa_settings.tcl                ;# part run_impl synth_directive impl_directive

    set latency  2                         ;# defaults a design.tcl may override
    set ii       1
    set elements ""
    source $ddir/design.tcl                ;# top srcs variants (+ latency ii elements)
    foreach v {top srcs variants} {
        if {![info exists $v]} { error "$ddir/design.tcl must set '$v'" }
    }

    set src_paths [lmap f $srcs {file join $ddir $f}]
    set xdc [expr {[file exists $ddir/timing.xdc] ? "$ddir/timing.xdc" : "timing.xdc"}]

    set stamp [clock format [clock seconds] -format %Y%m%d_%H%M%S]
    set date  [clock format [clock seconds] -format "%Y-%m-%d %H:%M"]
    set viv   [version -short]

    if {[catch {exec git rev-parse --short HEAD} commit]} {
        set commit "nogit"
    } elseif {![catch {exec git status --porcelain -- $ddir} dirty] && $dirty ne ""} {
        append commit "-dirty"
    }

    # ---------- results file ----------
    set csv results.csv
    set header [join $PPA_HEADER ,]
    if {[file exists $csv]} {
        set fh [open $csv r]; gets $fh first; close $fh
        if {$first ne $header} {
            set old results_old_$stamp.csv
            file rename $csv $old
            puts "NOTE: results.csv had different columns, moved it to $old"
        }
    }
    if {![file exists $csv]} {
        set fh [open $csv w]; puts $fh $header; close $fh
    }

    # ---------- run each variant ----------
    set summary {}
    set ran 0
    foreach v $variants {
        set label [lindex $v 0]
        set gens  [lindex $v 1]
        set lat   [expr {[llength $v] > 2 ? [lindex $v 2] : $latency}]
        if {[llength $only] && $label ni $only} { continue }
        incr ran

        set gd [dict create]
        foreach g $gens {
            set kv [split $g =]
            dict set gd [lindex $kv 0] [lindex $kv 1]
        }
        set elems [expr {$elements ne "" ? $elements : [ppa_dget $gd N]}]

        set rdir runs/${stamp}_${design}_$label
        file mkdir $rdir/src
        file copy -force {*}[glob -directory $ddir *] $xdc ppa_settings.tcl $rdir/src

        puts "\n========== $design / $label  ($gens) =========="

        foreach k {stage period wns fmax gelem luts ffs dsps carry lpe eff levels dlogic dnet} {
            set $k NA
        }
        set status OK

        if {[catch {
            create_project -in_memory -part $part
            read_verilog -sv $src_paths
            read_xdc $xdc

            set gargs {}
            foreach g $gens { lappend gargs -generic $g }
            synth_design -top $top -part $part -mode out_of_context \
                -directive $synth_directive {*}$gargs
            set stage synth

            if {$run_impl} {
                opt_design   -directive $impl_directive
                place_design -directive $impl_directive
                route_design -directive $impl_directive
                set stage impl
            }

            # timing: worst setup path
            set period [get_property PERIOD [get_clocks -quiet clk]]
            set path   [get_timing_paths -setup -max_paths 1 -nworst 1]
            if {$path ne "" && [ppa_num $period]} {
                set wns    [get_property SLACK $path]
                set fmax   [format %.1f [expr {1000.0 / ($period - $wns)}]]
                set levels [get_property LOGIC_LEVELS $path]
                set dlogic [get_property DATAPATH_LOGIC_DELAY $path]
                set dnet   [get_property DATAPATH_NET_DELAY $path]
            }

            # area (7-series "Slice ..." and UltraScale "CLB ..." wording)
            set util  [report_utilization -return_string]
            set luts  [ppa_grab $util {\|\s*(?:Slice|CLB) LUTs\*?\s*\|\s*(\d+)}]
            set ffs   [ppa_grab $util {\|\s*(?:Slice|CLB) Registers\*?\s*\|\s*(\d+)}]
            set dsps  [ppa_grab $util {\|\s*DSPs\*?\s*\|\s*(\d+)}]
            set carry [llength [get_cells -quiet -hierarchical -filter {PRIMITIVE_TYPE =~ CLB.CARRY.*}]]

            # throughput and efficiency
            if {[ppa_num $fmax] && [ppa_num $elems]} {
                set gelem [format %.2f [expr {$fmax * $elems / double($ii) / 1000.0}]]
            }
            if {[ppa_num $luts] && [ppa_num $elems] && $elems > 0} {
                set lpe [format %.1f [expr {double($luts) / $elems}]]
            }
            if {[ppa_num $gelem] && [ppa_num $luts] && $luts > 0} {
                set eff [format %.3f [expr {$gelem * 1000.0 / $luts}]]
            }

            report_timing_summary -max_paths 10 -file $rdir/timing_summary.rpt
            report_utilization -file $rdir/util.rpt
            report_utilization -hierarchical -file $rdir/util_hier.rpt
            write_checkpoint -force $rdir/$stage.dcp
        } err]} {
            set status FAILED
            puts "ERROR in $label: $err"
            set fh [open $rdir/error.txt w]; puts $fh $err; close $fh
        }
        close_project -quiet

        set row [list $date $design $label \
                      [ppa_dget $gd EXP] [ppa_dget $gd MAN] [ppa_dget $gd N] [ppa_dget $gd STAGES] \
                      [ppa_csv $gens] $lat $ii $elems \
                      $part $viv $synth_directive $impl_directive $stage $period $wns $fmax \
                      $gelem $luts $ffs $dsps $carry $lpe $eff \
                      $levels $dlogic $dnet $status $commit $rdir [ppa_csv $note]]
        set fh [open $csv a]; puts $fh [join $row ,]; close $fh

        lappend summary [format "  %-14s Fmax %7s MHz   LUTs %6s   FFs %6s   DSPs %4s   %s" \
                                $label $fmax $luts $ffs $dsps $status]
    }

    if {$ran == 0} {
        error "No variants matched '$only'. Labels in $ddir/design.tcl: [lmap v $variants {lindex $v 0}]"
    }
    puts "\n========== $design done: $ran variant(s) added to results.csv =========="
    puts [join $summary \n]
}

# ---------- entry point ----------
if {[info exists argc] && $argc > 0} {
    set design [lindex $argv 0]
    set note   [expr {$argc > 1 ? [lindex $argv 1] : ""}]
    set only   [expr {$argc > 2 ? [split [lindex $argv 2] ,] : ""}]
}
if {![info exists design]} {
    error "No design given. Example:  set design mx_comb; set note \"baseline\"; source run_ppa.tcl"
}
if {![info exists note]} { set note "" }
if {![info exists only]} { set only "" }

set ppa_err [catch {ppa_run $design $note $only} ppa_msg]
unset -nocomplain design note only     ;# so the next run in the same GUI session starts clean
if {$ppa_err} { error $ppa_msg }
