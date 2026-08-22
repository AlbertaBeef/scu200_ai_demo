# Live-reload the retrained MNIST weight/param blobs into DDR - no bitstream or ELF rebuild.
# The mbdpu01 core is weight-agnostic (reads the blobs from DDR each inference), so this alone
# deploys a freshly retrained model. Run after train_mnist_real.py regenerates the blobs.
#   xsct scripts/reload_weights.tcl
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
dow -data $root/train/mbdpu01/mnist_wblob.bin 0xB0000000
dow -data $root/train/mbdpu01/mnist_pblob.bin 0xB0010000
puts "=== retrained MNIST weights live-reloaded to DDR (0xB0000000 / 0xB0010000) ==="
exit
