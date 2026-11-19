# Hot-reload the app ELF only (FPGA .pdi + DDR blobs left untouched) - fast edit/rebuild loop.
#   xsct scripts/reload_elf.tcl [target]   (default scu200_ai_demo01)
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
source $here/target.tcl

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
stop
rst -processor
dow $elf
con
puts "=== ELF hot-reloaded (bitstream + DDR blobs untouched) ==="
exit
