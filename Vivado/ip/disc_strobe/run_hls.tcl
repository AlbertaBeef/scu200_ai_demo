# disc_strobe HLS build — free-running encoder-decode + camera strobe (ap_ctrl_none).
set board_name $::env(BOARD_NAME)
source "build/${board_name}/settings.tcl"
set curr_dir [pwd]
set SRC [file normalize [file join $curr_dir "disc_strobe"]]
set PROJ "disc_strobe.prj"
set SOLN "sol1"
if {![info exists CLKP]} { set CLKP 10.0 }
cd build/$board_name
open_project -reset $PROJ
add_files "${SRC}/disc_strobe.cpp" -cflags "-std=c++0x" -csimflags "-std=c++0x"
set_top disc_strobe
open_solution -reset $SOLN
if { $board_name == "scu200" } { enable_beta_device $XPART }
set_part $XPART
create_clock -period $CLKP
csynth_design
export_design -rtl verilog -format ip_catalog
cd ../..
exit
