# Hot-reload the app ELF only (FPGA .pdi + DDR blobs left untouched) - fast edit/rebuild loop.
#   xsct scripts/reload_elf.tcl
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
set elf  $root/Vitis/scu200_ai_demo01_workspace/test_app/build/test_app.elf

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
stop
rst -processor
dow $elf
con
puts "=== ELF hot-reloaded (bitstream + DDR blobs untouched) ==="
exit
