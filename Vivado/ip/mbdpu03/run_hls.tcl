# mbdpu03 HLS build — SCU200 AI demo custom DPU (112x112 mono INT8 CNN + position head, URAM).
# SPDX-License-Identifier: Apache-2.0
# Invoked by ip/Makefile as:
#   BOARD_NAME=<board> vitis-run --mode hls --tcl mbdpu03/run_hls.tcl

set board_name $::env(BOARD_NAME)

# settings.tcl (created by get_part.tcl) defines $XPART for this board
source "build/${board_name}/settings.tcl"

set curr_dir [pwd]
set SRC [file normalize [file join $curr_dir "mbdpu03"]]

set PROJ "mbdpu03.prj"
set SOLN "sol1"

# 10 ns = 100 MHz (same domain as mbdpu01/02). Override with CLKP (e.g. 5.0 for clk_200M).
if {![info exists CLKP]} {
  set CLKP 10.0
}

cd build/$board_name

open_project -reset $PROJ
add_files "${SRC}/mbdpu03.cpp" -cflags "-std=c++14" -csimflags "-std=c++14"
add_files -tb "${SRC}/mbdpu03_tb.cpp" -cflags "-std=c++14 -I${SRC}" -csimflags "-std=c++14"
set_top mbdpu03

open_solution -reset $SOLN

# xcsu200p is a beta device in 2025.2; enable it before set_part (as with mbdpu01/mbdpu02).
if { $board_name == "scu200" } {
  enable_beta_device $XPART
}
set_part $XPART
create_clock -period $CLKP

# C-sim first: random-weight + camera-crop tests, and (when present) the trainer's vectors in
# train/mbdpu03/ replayed bit-exactly against the Python INT8 model's expected outputs.
set ::env(MBDPU03_TESTDIR) [file normalize [file join $curr_dir ".." ".." "train" "mbdpu03"]]
csim_design

csynth_design
export_design -rtl verilog -format ip_catalog

# HLS 2025.2 cannot target URAM on the beta xcsu200p (HLS 214-370): the uram_* arrays are
# generated as BRAM and their exported RTL is rewritten here so Vivado infers UltraRAM.
set impl [file join [pwd] $PROJ $SOLN impl]
puts [exec python3 ${SRC}/patch_uram.py [file join $impl verilog] [file join $impl ip hdl verilog]]

cd ../..

exit
