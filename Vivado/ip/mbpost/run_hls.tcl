# mbpost HLS build - SCU200 overlay renderer (scu200_ai_demo03): preview + logit bars + DX/DY
# gauges drawn straight into the HDMI background frame buffer in DDR.
# SPDX-License-Identifier: Apache-2.0
# Modeled on mbpre/run_hls.tcl. Invoked by ip/Makefile as:
#   BOARD_NAME=<board> vitis-run --mode hls --tcl mbpost/run_hls.tcl

set board_name $::env(BOARD_NAME)

# settings.tcl (created by get_part.tcl) defines $XPART for this board
source "build/${board_name}/settings.tcl"

set curr_dir [pwd]
set SRC [file normalize [file join $curr_dir "mbpost"]]

set PROJ "mbpost.prj"
set SOLN "sol1"

# 10 ns = 100 MHz, the same domain as mbdpu03 (a latency-tolerant DDR client, not in the video path).
if {![info exists CLKP]} {
  set CLKP 10.0
}

cd build/$board_name

open_project -reset $PROJ
add_files "${SRC}/mbpost.cpp" -cflags "-std=c++14" -csimflags "-std=c++14"
add_files -tb "${SRC}/mbpost_tb.cpp" -cflags "-std=c++14 -I${SRC}" -csimflags "-std=c++14"
set_top mbpost

open_solution -reset $SOLN

# xcsu200p is a beta device in 2025.2; enable it before set_part (as with mbpre/mbdpu0x).
if { $board_name == "scu200" } {
  enable_beta_device $XPART
}
set_part $XPART
create_clock -period $CLKP

csim_design
csynth_design
export_design -rtl verilog -format ip_catalog

cd ../..

exit
