# Live-reload the retrained MNIST weight/param blobs into DDR - no bitstream or ELF rebuild.
# The mbdpu01 core is weight-agnostic (reads the blobs from DDR each inference), so this alone
# deploys a freshly retrained model. Run after train_mnist_real.py regenerates the blobs.
#   xsct scripts/reload_weights.tcl [target]   (default scu200_ai_demo01)
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
source $here/target.tcl

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
dow -data $wblob 0xB0000000
dow -data $pblob 0xB0010000
puts "=== retrained MNIST weights live-reloaded to DDR (0xB0000000 / 0xB0010000) ==="
exit
