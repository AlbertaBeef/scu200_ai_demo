# target.tcl — shared by the JTAG scripts: resolve the build target (first script argument,
# default scu200_ai_demo01) into the files that get programmed / reloaded.
#   xsct scripts/program.tcl                     -> scu200_ai_demo01 (mbdpu01 weights)
#   xsct scripts/program.tcl scu200_ai_demo02    -> scu200_ai_demo02 (mbdpu02 weights + dx/dy head)
#   xsct scripts/program.tcl scu200_ai_demo03    -> scu200_ai_demo03 (mbdpu03 112x112 weights)
# Sets: target, pdi, elf, bg, wblob, pblob (all absolute paths under $root).
set target [expr {$argc > 0 ? [lindex $argv 0] : "scu200_ai_demo01"}]
set core_of [dict create scu200_ai_demo01 mbdpu01 scu200_ai_demo02 mbdpu02 scu200_ai_demo03 mbdpu03]
if {![dict exists $core_of $target]} {
    puts "ERROR: unknown target '$target' (expected one of: [dict keys $core_of])"
    exit 1
}
set core  [dict get $core_of $target]
set pdi   $root/Vivado/$target/$target.runs/impl_1/${target}_wrapper.pdi
set elf   $root/Vitis/${target}_workspace/test_app/build/test_app.elf
set bg    $root/assets/${target}_background_rgb.bin
set wblob $root/train/$core/mnist_wblob.bin
set pblob $root/train/$core/mnist_pblob.bin
puts "target: $target  (weights: train/$core)"
