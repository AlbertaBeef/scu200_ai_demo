# Program a demo design over JTAG: FPGA (.pdi) + app ELF + background + weights.
#
#   xsct scripts/program.tcl [target]   (default scu200_ai_demo01; or scu200_ai_demo02)
#   (run from the repo root, or anywhere - paths are relative to the repo)
#
# Prereqs: Stage 1 (Vivado -> .pdi) and Stage 2 (Vitis -> test_app.elf) built; the background
# blob generated (tools/png_to_rgb888.py). DDR map: bg 0xB4000000, weights 0xB0000000,
# params 0xB0010000 (see README). Power-cycle the board before re-programming.
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

source $here/target.tcl

connect
device program $pdi
after 2000
targets -set -nocase -filter {name =~ "*MicroBlaze*#0*"}
rst -processor
dow $elf
if {[file exists $bg]} {
    dow -data $bg 0xB4000000
} else {
    puts "WARN: $bg missing - run 'python3 tools/png_to_rgb888.py --png assets/${target}_background.png --out $bg' (background will be blank)"
}
dow -data $wblob 0xB0000000
dow -data $pblob 0xB0010000
con
puts "=== PROGRAMMED (PDI + ELF + bg + weights) ==="
exit
