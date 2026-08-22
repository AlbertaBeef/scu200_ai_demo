# Dump the ROI training-data gallery from DDR (0xBA000000) to a host .bin over JTAG.
# Run AFTER recording (press 'w' to arm/stop capture, spin the disc): xsct scripts/dump_gallery.tcl
# The MicroBlaze can stay running - the gallery is static once recording stops.
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

connect
targets -set -nocase -filter {name =~ "MicroBlaze*#0"}
set base 0xBA000000
set magic  [lindex [mrd -force -value $base 1] 0]
set count  [lindex [mrd -force -value [expr $base + 0x04] 1] 0]
set stride [lindex [mrd -force -value [expr $base + 0x0C] 1] 0]
set galhdr [lindex [mrd -force -value [expr $base + 0x10] 1] 0]
puts "gallery: magic=0x$magic  count=$count  stride=$stride  hdr=$galhdr"
if {$count == 0} { puts "no samples recorded (press 'w', arm F+S, spin, then re-dump)"; exit }
set bytes [expr $galhdr + $count * $stride]
set words [expr ($bytes + 3) / 4]
set out $root/train/captures/roi_gallery.bin
mrd -force -bin -file $out $base $words
puts "dumped $bytes bytes ($count samples) -> $out"
puts "next: python3 tools/roi_export.py --bin train/captures/roi_gallery.bin \\"
puts "        --out train/captures/mnist_takeNN --map 1,2,3,4,5,6,7,8"
exit
