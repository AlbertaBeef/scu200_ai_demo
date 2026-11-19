# Hot-reload the app ELF and refresh the background blob (FPGA .pdi untouched).
# Use after a rebuild that also changed the background art.
#   xsct scripts/reload_elf_bg.tcl [target]   (default scu200_ai_demo01)
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
source $here/target.tcl

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
stop
rst -processor
dow $elf
if {[file exists $bg]} { dow -data $bg 0xB4000000 }
con
puts "=== ELF reloaded + pristine bg refreshed @0xB4000000 (PDI untouched) ==="
exit
