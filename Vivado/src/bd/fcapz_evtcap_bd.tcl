#########################################################
# fcapz event-timeline capture (scu200_ai_demo03_fcapz only)
#########################################################
# Sourced by bd_scu200_ai_demo03.tcl when $fcapz_en is set, before the AXI-Lite
# interconnect is built (so fcapz_mark joins periph_ports).
#
#  - fcapz_mark   : AXI GPIO, ch1 = main-loop markers, ch2 = ISR markers (8 bits each).
#                   Two channels so the ISR and the main loop never read-modify-write
#                   the same register.
#  - fcapz_evtcap : module reference (src/hdl/fcapz_evtcap.v) = AXI monitors on
#                   frmbuf_wr AW, display frmbuf_rd AR and mbdpu03 AR + fpgacapZero
#                   ELA (32 bit x 4K, timestamps, storage qualification) on BSCAN USER3.

set fcapz_hdl [file normalize [file join [file dirname [info script]] .. hdl]]
add_files -norecurse -fileset sources_1 [concat \
    [glob $fcapz_hdl/fcapz/*.v] \
    [list $fcapz_hdl/fcapz/fcapz_version.vh $fcapz_hdl/fcapz_evtcap.v]]
set_property file_type {Verilog Header} [get_files */fcapz/fcapz_version.vh]
set_property include_dirs [list $fcapz_hdl/fcapz] [get_filesets sources_1]
# (no update_compile_order here: with the wrapper not yet generated, automatic hierarchy
#  mode would pick fcapz_evtcap as the project top; build.tcl re-asserts the top.)

# --- firmware marker GPIO
set fcapz_mark [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio fcapz_mark ]
set_property -dict [list \
  CONFIG.C_GPIO_WIDTH {8} \
  CONFIG.C_ALL_OUTPUTS {1} \
  CONFIG.C_IS_DUAL {1} \
  CONFIG.C_GPIO2_WIDTH {8} \
  CONFIG.C_ALL_OUTPUTS_2 {1} \
] $fcapz_mark
connect_bd_net [get_bd_pins clk_wiz/clk_100M]                    [get_bd_pins fcapz_mark/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins fcapz_mark/s_axi_aresetn]
lappend periph_ports [list "fcapz_mark/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]

# --- capture core
set fcapz_evtcap [ create_bd_cell -type module -reference fcapz_evtcap fcapz_evtcap ]
connect_bd_net [get_bd_pins clk_wiz/clk_100M]                    [get_bd_pins fcapz_evtcap/clk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins fcapz_evtcap/aresetn]
connect_bd_net [get_bd_pins $cam_clk]                            [get_bd_pins fcapz_evtcap/vclk]

# AXI monitors. The frame buffers live inside the mipi_0 hierarchy: give it Monitor
# interface pins attached to the existing m_axi nets, then connect those out.
create_bd_intf_pin -mode Monitor -vlnv xilinx.com:interface:aximm_rtl:1.0 mipi_0/MON_FRMBUF_WR
create_bd_intf_pin -mode Monitor -vlnv xilinx.com:interface:aximm_rtl:1.0 mipi_0/MON_FRMBUF_RD
connect_bd_intf_net [get_bd_intf_pins mipi_0/MON_FRMBUF_WR] [get_bd_intf_pins mipi_0/v_frmbuf_wr/m_axi_mm_video]
connect_bd_intf_net [get_bd_intf_pins mipi_0/MON_FRMBUF_RD] [get_bd_intf_pins mipi_0/v_frmbuf_rd/m_axi_mm_video]
connect_bd_intf_net [get_bd_intf_pins mipi_0/MON_FRMBUF_WR] [get_bd_intf_pins fcapz_evtcap/MON_WR]
connect_bd_intf_net [get_bd_intf_pins mipi_0/MON_FRMBUF_RD] [get_bd_intf_pins fcapz_evtcap/MON_RD]
connect_bd_intf_net [get_bd_intf_pins fcapz_evtcap/MON_DPU] [get_bd_intf_pins mbdpu03/m_axi_gmem]

# single-bit events
connect_bd_net [get_bd_pins disc_strobe/xvs]       [get_bd_pins fcapz_evtcap/fsin]
connect_bd_net [get_bd_pins mipi_0/frmbufwr_irq]   [get_bd_pins fcapz_evtcap/wr_irq]
connect_bd_net [get_bd_pins mipi_0/frmbufrd_irq]   [get_bd_pins fcapz_evtcap/rd_irq]
connect_bd_net [get_bd_pins fcapz_mark/gpio_io_o]  [get_bd_pins fcapz_evtcap/mark_a]
connect_bd_net [get_bd_pins fcapz_mark/gpio2_io_o] [get_bd_pins fcapz_evtcap/mark_b]
