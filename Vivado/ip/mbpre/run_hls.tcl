# mbpre HLS build — SCU200 custom DPU, milestone 2 (quantized MNIST CNN, INT8).
# SPDX-License-Identifier: Apache-2.0
# Modeled on mbdpu01/run_hls.tcl. Invoked by ip/Makefile as:
#   BOARD_NAME=<board> vitis-run --mode hls --tcl mbpre/run_hls.tcl

set board_name $::env(BOARD_NAME)

# settings.tcl (created by get_part.tcl) defines $XPART for this board
source "build/${board_name}/settings.tcl"

set curr_dir [pwd]
set SRC [file normalize [file join $curr_dir "mbpre"]]

set PROJ "mbpre.prj"
set SOLN "sol1"

# 10 ns = 100 MHz. mbpre is a decoupled, latency-tolerant DDR/compute client (not in
# the video path), so it runs in the 100 MHz domain — the unoptimized conv datapath does
# NOT close 300 MHz (~-3.36 ns), but has huge margin at 100 MHz. Retune to 300 MHz later
# with pipeline/unroll if throughput demands it.
if {![info exists CLKP]} {
  set CLKP 10.0
}

cd build/$board_name

open_project -reset $PROJ
add_files "${SRC}/mbpre.cpp" -cflags "-std=c++0x" -csimflags "-std=c++0x"
set_top mbpre

open_solution -reset $SOLN

# xcsu200p is a beta device in 2025.2; enable it before set_part (as with mbdpu01).
if { $board_name == "scu200" } {
  enable_beta_device $XPART
}
set_part $XPART
create_clock -period $CLKP

csynth_design
export_design -rtl verilog -format ip_catalog

cd ../..

exit
