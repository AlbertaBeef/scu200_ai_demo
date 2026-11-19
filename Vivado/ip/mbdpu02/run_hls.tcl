# mbdpu02 HLS build — SCU200 AI demo custom DPU (INT8 MNIST CNN + digit-position head).
# SPDX-License-Identifier: Apache-2.0
# Invoked by ip/Makefile as:
#   BOARD_NAME=<board> vitis-run --mode hls --tcl mbdpu02/run_hls.tcl

set board_name $::env(BOARD_NAME)

# settings.tcl (created by get_part.tcl) defines $XPART for this board
source "build/${board_name}/settings.tcl"

set curr_dir [pwd]
set SRC [file normalize [file join $curr_dir "mbdpu02"]]

set PROJ "mbdpu02.prj"
set SOLN "sol1"

# 10 ns = 100 MHz. mbdpu02 (like mbdpu01) is a decoupled, latency-tolerant DDR/compute client (not in
# the video path), so it runs in the 100 MHz domain — the unoptimized conv datapath does
# NOT close 300 MHz (~-3.36 ns), but has huge margin at 100 MHz. Retune to 300 MHz later
# with pipeline/unroll if throughput demands it.
if {![info exists CLKP]} {
  set CLKP 10.0
}

cd build/$board_name

open_project -reset $PROJ
add_files "${SRC}/mbdpu02.cpp" -cflags "-std=c++0x" -csimflags "-std=c++0x"
add_files -tb "${SRC}/mbdpu02_tb.cpp" -cflags "-std=c++0x -I${SRC}" -csimflags "-std=c++0x"
set_top mbdpu02

open_solution -reset $SOLN

# xcsu200p is a beta device in 2025.2; enable it before set_part (as with mbdpu01/mbdpu02).
if { $board_name == "scu200" } {
  enable_beta_device $XPART
}
set_part $XPART
create_clock -period $CLKP

# C-sim first: random-weight datapath test + (when present) the trainer's vectors in
# train/mbdpu02/ replayed bit-exactly against the Python INT8 model's expected outputs.
set ::env(MBDPU02_TESTDIR) [file normalize [file join $curr_dir ".." ".." "train" "mbdpu02"]]
csim_design

csynth_design
export_design -rtl verilog -format ip_catalog

cd ../..

exit
