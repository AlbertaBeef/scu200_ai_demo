# Hot-reload the app ELF and refresh the background blob (FPGA .pdi untouched).
# Use after a rebuild that also changed the background art.
#   xsct scripts/reload_elf_bg.tcl
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
set elf  $root/Vitis/scu200_ai_demo01_workspace/test_app/build/test_app.elf
set bg   $root/assets/scu200_ai_demo01_background_rgb.bin

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
stop
rst -processor
dow $elf
if {[file exists $bg]} { dow -data $bg 0xB4000000 }
con
puts "=== ELF reloaded + pristine bg refreshed @0xB4000000 (PDI untouched) ==="
exit
