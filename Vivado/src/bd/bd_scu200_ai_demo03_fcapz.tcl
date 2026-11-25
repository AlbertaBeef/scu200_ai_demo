# scu200_ai_demo03_fcapz: scu200_ai_demo03 + fpgacapZero event-timeline capture.
# Same block design as demo03; fcapz_en makes it source fcapz_evtcap_bd.tcl
# (marker GPIO + AXI monitors + ELA on BSCAN USER3) before the interconnect is built.
set fcapz_en 1
source [file join [file dirname [info script]] bd_scu200_ai_demo03.tcl]
