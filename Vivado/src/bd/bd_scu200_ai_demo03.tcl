# NOTE (SCU200): derived from bd_mb.tcl. Only change vs bd_mb.tcl is that the
# three per-pipe interconnects (axi_int_ctrl/video/mem) use AXI SmartConnect
# instead of the legacy axi_interconnect IP, which is unsupported on XCSU200P.
################################################################
# Block diagram build script for MicroBlaze designs
################################################################

################################################################
# AXI GPIOs are connected as follows:
################################################################
#
# Main AXI GPIO
#
# 0    - Test Pattern Generator reset
# 1    - Video Mixer reset
# 2-31 - Reserved
#
# Video pipe AXI GPIO (one for each pipe)
#
# 0    - Camera GPIO0
# 1    - Camera GPIO1
# 2    - Demosaic IP reset
# 3    - Vproc IP reset
# 4    - Gamma LUT IP reset
# 5    - Frame Buffer Read IP reset
# 6    - Frame Buffer Write IP reset
# 7-31 - Reserved

# CHECKING IF PROJECT EXISTS
if { [get_projects -quiet] eq "" } {
   puts "ERROR: Please open or create a project!"
   return 1
}

set cur_design [current_bd_design -quiet]
set list_cells [get_bd_cells -quiet]

create_bd_design $block_name

current_bd_design $block_name

set parentCell [get_bd_cells /]

# Get object for parentCell
set parentObj [get_bd_cells $parentCell]
if { $parentObj == "" } {
   puts "ERROR: Unable to find parent cell <$parentCell>!"
   return
}

# Make sure parentObj is hier blk
set parentType [get_property TYPE $parentObj]
if { $parentType ne "hier" } {
   puts "ERROR: Parent <$parentObj> has TYPE = <$parentType>. Expected to be <hier>."
   return
}

# Save current instance; Restore later
set oldCurInst [current_bd_instance .]

# Set parent object as current
current_bd_instance $parentObj

# Number of cameras
set num_cams [llength $cams]

# Clock and reset selections
set cam_clk clk_wiz/clk_300M
set cam_rst rst_clk_wiz_300M

# Video pipe max res has impact on resource usage
set max_cols 1920
set max_rows 1232

# Set the samples-per-clock for the video pipelines (possible values 1,2,4)
# Increasing this value increases bandwidth and resource usage
set pipe_samples_pc 1

# Set the samples-per-clock for the display pipeline (possible values 2,4)
# Increasing this value increases bandwidth and resource usage
# NOTE(scu200): v_hdmi_txss1 (HDMI 2.1 FRL) only accepts 4 or 8 ppc, so the whole
# display pipeline (v_tpg/v_mix/frmbuf_rd/txss) runs at 4 ppc here (was 2 on auboard).
set disp_samples_pc 4

# AXI Lite ports
set periph_ports {}

# List of interrupt pins (AXI Intc)
set intr_list {}

# Create interface ports
set HDMI_CLK_IIC [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 HDMI_CLK_IIC ]

set TX_DDC_OUT [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 TX_DDC_OUT ]

set RS232_UART [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:uart_rtl:1.0 RS232_UART ]

set GTH_CLK_IIC [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 GTH_CLK_IIC ]


# Create ports
set TX_HPD_IN [ create_bd_port -dir I TX_HPD_IN ]
set HDMI_TX_DAT_P_OUT [ create_bd_port -dir O -from 3 -to 0 HDMI_TX_DAT_P_OUT ]
set HDMI_TX_DAT_N_OUT [ create_bd_port -dir O -from 3 -to 0 HDMI_TX_DAT_N_OUT ]
set IDT_8T49N241_LOL_IN [ create_bd_port -dir I IDT_8T49N241_LOL_IN ]
set LED0 [ create_bd_port -dir O LED0 ]
# SCU200 board pin HDMI_TX_ENABLE_N is ACTIVE-LOW -> driven from gnd_const (0) to enable.
set TX_EN_OUT [ create_bd_port -dir O -from 0 -to 0 TX_EN_OUT ]
set TX_REFCLK_P_IN [ create_bd_port -dir I -type clk TX_REFCLK_P_IN ]
set TX_REFCLK_N_IN [ create_bd_port -dir I -type clk TX_REFCLK_N_IN ]
set LED1 [ create_bd_port -dir O -from 0 -to 0 LED1 ]
set LED2 [ create_bd_port -dir O LED2 ]
set IDT_8T49N241_RST_OUT [ create_bd_port -dir O -from 0 -to 0 -type rst IDT_8T49N241_RST_OUT ]
# HDMI 2.1 PHY DRU / fixed GT reference clock (~400 MHz from board SiT95211)
set DRU_CLK_IN [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 DRU_CLK_IN ]

# Option A background (opt-4): the v_tpg cross-hatch is REPLACED by bg_frmbuf_rd (a
# v_frmbuf_rd streaming a static image from DDR into the v_mix master). That cell is
# created after the camera loop; the reset_v_tpg slice below is retained and reused
# for bg_frmbuf_rd/ap_rst_n (so the same RESET_MASK_VTPG software control applies).

# Slice for Test Pattern Generator reset signal
set resetn_index 0
set reset_v_tpg [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 reset_v_tpg ]
set_property -dict [ list \
CONFIG.DIN_WIDTH {32} \
CONFIG.DIN_TO $resetn_index \
CONFIG.DIN_FROM $resetn_index \
CONFIG.DOUT_WIDTH {1} \
] $reset_v_tpg

# (reset_v_tpg/Dout now drives bg_frmbuf_rd/ap_rst_n — connected at its creation below)

# Add the Video mixer
set v_mix [create_bd_cell -type ip -vlnv xilinx.com:ip:v_mix v_mix]
set num_v_mix_ports [expr {$num_cams+1}]
set_property -dict [ list \
 CONFIG.SAMPLES_PER_CLOCK $disp_samples_pc \
 CONFIG.AXIMM_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO1_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO2_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO3_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO4_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO5_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO6_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO7_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO8_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO9_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO10_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO11_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO12_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO13_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO14_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO15_DATA_WIDTH {128} \
 CONFIG.C_M_AXI_MM_VIDEO16_DATA_WIDTH {128} \
 CONFIG.LAYER1_INTF_TYPE {1} \
 CONFIG.LAYER1_UPSAMPLE {false} \
 CONFIG.LAYER1_VIDEO_FORMAT {0} \
 CONFIG.LAYER2_INTF_TYPE {1} \
 CONFIG.LAYER2_UPSAMPLE {false} \
 CONFIG.LAYER2_VIDEO_FORMAT {0} \
 CONFIG.LAYER3_INTF_TYPE {1} \
 CONFIG.LAYER3_UPSAMPLE {false} \
 CONFIG.LAYER3_VIDEO_FORMAT {0} \
 CONFIG.LAYER4_INTF_TYPE {1} \
 CONFIG.LAYER4_VIDEO_FORMAT {0} \
 CONFIG.MAX_COLS {1920} \
 CONFIG.MAX_ROWS {1080} \
 CONFIG.NR_LAYERS $num_v_mix_ports \
] $v_mix

# Slice for Video Mixer reset signal
set resetn_index 1
set reset_v_mix [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 reset_v_mix ]
set_property -dict [ list \
CONFIG.DIN_WIDTH {32} \
CONFIG.DIN_TO $resetn_index \
CONFIG.DIN_FROM $resetn_index \
CONFIG.DOUT_WIDTH {1} \
] $reset_v_mix

connect_bd_net [get_bd_pins reset_v_mix/Dout] [get_bd_pins v_mix/ap_rst_n]

# Create instance: tx_video_axis_reg_slice, and set properties
set tx_video_axis_reg_slice [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice tx_video_axis_reg_slice ]

# Create instance: v_hdmi_txss1 (HDMI 2.1 TX Subsystem), and set properties
set v_hdmi_txss1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:v_hdmi_txss1:1.2 v_hdmi_txss1 ]
set_property -dict [list \
  CONFIG.C_ADDR_WIDTH {10} \
  CONFIG.C_ADD_CORE_DBG {0} \
  CONFIG.C_ADD_MARK_DBG {false} \
  CONFIG.C_DYNAMIC_HDR {0} \
  CONFIG.C_EXDES_RX_PLL_SELECTION {8} \
  CONFIG.C_EXDES_TX_PLL_SELECTION {7} \
  CONFIG.C_FRL_CLK_FREQ_KHZ {150000} \
  CONFIG.C_HDMI_MODE {0} \
  CONFIG.C_HPD_INVERT {true} \
  CONFIG.C_HYSTERESIS_LEVEL {511} \
  CONFIG.C_INCLUDE_HDCP {false} \
  CONFIG.C_INCLUDE_HDCP_1_4 {false} \
  CONFIG.C_INCLUDE_HDCP_2_2 {false} \
  CONFIG.C_INPUT_PIXELS_PER_CLOCK $disp_samples_pc \
  CONFIG.C_MAX_BITS_PER_COMPONENT {8} \
  CONFIG.C_MAX_FRL_RATE {0} \
  CONFIG.C_VALIDATION_ENABLE {false} \
  CONFIG.C_VID_CLK_FREQ_KHZ {150000} \
  CONFIG.C_VID_INTERFACE {0} \
  CONFIG.C_VRR_SUPPORT {0} \
] $v_hdmi_txss1


# Create instance: vcc_const, and set properties
set vcc_const [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconstant:1.0 vcc_const ]
set_property CONFIG.CONST_VAL {1} $vcc_const

# Create instance: gnd_const, and set properties
set gnd_const [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconstant:1.0 gnd_const ]
set_property CONFIG.CONST_VAL {0} $gnd_const
connect_bd_net [get_bd_pins gnd_const/dout] [get_bd_pins v_hdmi_txss1/fid] [get_bd_ports TX_EN_OUT]


# Create instance: v_hdmi_phy1 (HDMI PHY Controller, TX-only), and set properties
set v_hdmi_phy1_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:v_hdmi_phy1:1.0 v_hdmi_phy1_0 ]
set_property -dict [list \
  CONFIG.Adv_Clk_Mode {false} \
  CONFIG.C_INPUT_PIXELS_PER_CLOCK $disp_samples_pc \
  CONFIG.C_RX_REFCLK_SEL {0} \
  CONFIG.C_Rx_Protocol {None} \
  CONFIG.C_TX_FRL_REFCLK_SEL {5} \
  CONFIG.C_TX_REFCLK_SEL {1} \
  CONFIG.C_Txrefclk_Rdy_Invert {true} \
  CONFIG.C_user_loss {14} \
  CONFIG.Rx_GT_Line_Rate {8.0} \
  CONFIG.Rx_Max_GT_Line_Rate {8.0} \
  CONFIG.Tx_GT_Line_Rate {8.0} \
  CONFIG.Tx_Max_GT_Line_Rate {8.0} \
] $v_hdmi_phy1_0

# GT reference-clock buffering for the HDMI PHY (DRU_CLK_IN -> GT south refclk)
set util_ds_buf_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 util_ds_buf_0 ]
set_property CONFIG.C_BUF_TYPE {BUFG_GT} $util_ds_buf_0
set util_ds_buf_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 util_ds_buf_1 ]
set_property CONFIG.C_BUF_TYPE {IBUFDSGTE} $util_ds_buf_1


# Create instance: axi_uartlite, and set properties
set axi_uartlite [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_uartlite axi_uartlite ]
set_property -dict [list \
  CONFIG.C_BAUDRATE {115200} \
  CONFIG.UARTLITE_BOARD_INTERFACE {Custom} \
  CONFIG.USE_BOARD_FLOW {true} \
] $axi_uartlite

# Add the DDR
create_bd_cell -type ip -vlnv xilinx.com:ip:ddr4 ddr4_0
# SCU200 has no board file: configure the DDR4 controller manually (Custom board
# interfaces) for the on-board Micron MT40A512M16HA-083E, and create the external
# DDR4 + system-clock ports by hand. Config taken from the scu200_step01 project.
set_property -dict [list \
  CONFIG.C0_CLOCK_BOARD_INTERFACE {Custom} \
  CONFIG.C0_DDR4_BOARD_INTERFACE {Custom} \
  CONFIG.C0.DDR4_InputClockPeriod {3750} \
  CONFIG.C0.DDR4_MemoryPart {MT40A512M16HA-083E} \
  CONFIG.C0.DDR4_DataWidth {16} \
  CONFIG.C0.DDR4_AxiSelection {true} \
  CONFIG.ADDN_UI_CLKOUT1_FREQ_HZ {100} \
] [get_bd_cells ddr4_0]

# External DDR4 SDRAM interface port (pin LOCs come from the XDC, not a board file)
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 ddr4_sdram
connect_bd_intf_net [get_bd_intf_ports ddr4_sdram] [get_bd_intf_pins ddr4_0/C0_DDR4]

# External DDR4 differential system clock port (on-board CLK95211_OUT1_DDR4_SYSCLK).
# NOTE: this is 266.67 MHz on the SCU200 (UG1780 clock table), NOT 300 MHz. The
# port name is kept for XDC stability but is a misnomer. A 300 MHz assumption
# stops the DDR4 MMCM locking, which kills addn_ui_clkout1 -> clk_wiz -> MicroBlaze.
create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 system_clock_300mhz
set_property CONFIG.FREQ_HZ {266670000} [get_bd_intf_ports system_clock_300mhz]
connect_bd_intf_net [get_bd_intf_ports system_clock_300mhz] [get_bd_intf_pins ddr4_0/C0_SYS_CLK]

# Create instance: clk_wiz, and set properties
# Add and configure the clock wizard
set clk_wiz [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz clk_wiz ]
set_property -dict [list \
  CONFIG.OPTIMIZE_CLOCKING_STRUCTURE_EN {true} \
  CONFIG.CLKOUT1_JITTER {85.183} \
  CONFIG.CLKOUT1_PHASE_ERROR {76.968} \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {200.000} \
  CONFIG.CLKOUT2_JITTER {96.285} \
  CONFIG.CLKOUT2_PHASE_ERROR {76.968} \
  CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {100.000} \
  CONFIG.CLKOUT2_USED {true} \
  CONFIG.CLKOUT3_JITTER {79.342} \
  CONFIG.CLKOUT3_PHASE_ERROR {76.968} \
  CONFIG.CLKOUT3_REQUESTED_OUT_FREQ {300.000} \
  CONFIG.CLKOUT3_USED {true} \
  CONFIG.CLKOUT4_USED {true} \
  CONFIG.CLKOUT4_REQUESTED_OUT_FREQ {450.000} \
  CONFIG.CLKOUT5_USED {false} \
  CONFIG.MMCM_CLKFBOUT_MULT_F {15.000} \
  CONFIG.MMCM_CLKOUT0_DIVIDE_F {7.500} \
  CONFIG.MMCM_CLKOUT1_DIVIDE {15} \
  CONFIG.MMCM_CLKOUT2_DIVIDE {5} \
  CONFIG.MMCM_CLKOUT3_DIVIDE {30} \
  CONFIG.RESET_PORT {resetn} \
  CONFIG.RESET_TYPE {ACTIVE_LOW} \
  CONFIG.CLK_OUT1_PORT {clk_200M} \
  CONFIG.CLK_OUT2_PORT {clk_100M} \
  CONFIG.CLK_OUT3_PORT {clk_300M} \
  CONFIG.CLK_OUT4_PORT {clk_450M} \
  CONFIG.NUM_OUT_CLKS {4} \
] $clk_wiz

# Connect 100MHz DDR additional user clock output to Clk wizard
connect_bd_net [get_bd_pins ddr4_0/addn_ui_clkout1] [get_bd_pins clk_wiz/clk_in1]

# Connect clocks for the V_TPG and V_MIX
# (v_tpg removed; bg_frmbuf_rd/ap_clk = clk_300M is connected at its creation below)
connect_bd_net [get_bd_pins clk_wiz/clk_300M] [get_bd_pins v_mix/ap_clk]

# Manual active-low FPGA reset port (no board file -> no board automation)
create_bd_port -dir I -type rst system_resetn
set_property CONFIG.POLARITY {ACTIVE_LOW} [get_bd_ports system_resetn]
# clk_wiz reset tied deasserted (const 1) so the MMCM is NEVER held in reset by the
# external pushbutton. The MMCM self-resets on config; this removes CPU_RESET_B (B24)
# as a way to accidentally kill all fabric clocks. (DDR4 sys_rst still tracks the
# button via invert_rst below, now made reliable by the XDC pull-up on B24.)
connect_bd_net [get_bd_pins vcc_const/dout] [get_bd_pins clk_wiz/resetn]

# Reset for DDR
create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilvector_logic:1.0 invert_rst
set_property -dict [list \
  CONFIG.C_OPERATION {not} \
  CONFIG.C_SIZE {1} \
] [get_bd_cells invert_rst]
connect_bd_net [get_bd_ports system_resetn] [get_bd_pins invert_rst/Op1]
connect_bd_net [get_bd_pins invert_rst/Res] [get_bd_pins ddr4_0/sys_rst]

# Add and configure reset processor system for the 300MHz DDR clock
set rst_ddr_300M [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_ddr_300M ]
connect_bd_net [get_bd_pins ddr4_0/c0_ddr4_ui_clk] [get_bd_pins rst_ddr_300M/slowest_sync_clk]
connect_bd_net [get_bd_pins ddr4_0/c0_ddr4_ui_clk_sync_rst] [get_bd_pins rst_ddr_300M/ext_reset_in]
connect_bd_net [get_bd_pins rst_ddr_300M/peripheral_aresetn] [get_bd_pins ddr4_0/c0_ddr4_aresetn]

# Add the MicroBlaze
set microblaze_0 [create_bd_cell -type ip -vlnv xilinx.com:ip:microblaze:11.0 microblaze_0]
apply_bd_automation -rule xilinx.com:bd_rule:microblaze -config { all {1} axi_intc {1} axi_periph {Enabled} cache {32KB} clk {/clk_wiz/clk_100M (100 MHz)} compress {1} cores {1} debug_module {Debug Only} disable {0} ecc {None} local_mem {16KB} preset {Application}}  $microblaze_0
set_property -dict [list \
  CONFIG.C_DEBUG_ENABLED {1} \
  CONFIG.C_D_AXI {1} \
  CONFIG.C_D_LMB {1} \
  CONFIG.C_I_AXI {0} \
  CONFIG.C_I_LMB {1} \
] $microblaze_0

# Connect Microblaze to DDR
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config { Clk_master {/clk_wiz/clk_100M (100 MHz)} Clk_slave {/ddr4_0/c0_ddr4_ui_clk (300 MHz)} Clk_xbar {Auto} Master {/microblaze_0 (Cached)} Slave {/ddr4_0/C0_DDR4_S_AXI} ddr_seg {Auto} intc_ip {New AXI SmartConnect} master_apm {0}}  [get_bd_intf_pins ddr4_0/C0_DDR4_S_AXI]

# Create AXI IIC for the HDMI CLK generator
set axi_iic_hdmi_clk [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic axi_iic_hdmi_clk ]
set_property -dict [list \
  CONFIG.IIC_BOARD_INTERFACE {Custom} \
  CONFIG.USE_BOARD_FLOW {true} \
] $axi_iic_hdmi_clk

# Create instance: rst_clk_wiz_300M, and set properties
set rst_clk_wiz_300M [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_clk_wiz_300M ]
set_property -dict [list \
  CONFIG.RESET_BOARD_INTERFACE {Custom} \
  CONFIG.USE_BOARD_FLOW {false} \
] $rst_clk_wiz_300M

# Create AXI IIC for the GTH CLK generator
set axi_iic_gth_clk [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic axi_iic_gth_clk ]
set_property -dict [list \
  CONFIG.IIC_BOARD_INTERFACE {Custom} \
  CONFIG.USE_BOARD_FLOW {true} \
] $axi_iic_gth_clk

# Create AXI Timer
set axi_timer [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_timer axi_timer ]
set_property CONFIG.enable_timer2 {0} $axi_timer
connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins axi_timer/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins axi_timer/s_axi_aresetn]

# AXI Peripheral ports
lappend periph_ports [list "v_hdmi_phy1_0/vid_phy_axi4lite" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend periph_ports [list "bg_frmbuf_rd/s_axi_CTRL" "clk_wiz/clk_300M" "rst_clk_wiz_300M/peripheral_aresetn"]
lappend periph_ports [list "v_hdmi_txss1/S_AXI_CPU_IN" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend periph_ports [list "axi_uartlite/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend periph_ports [list "axi_iic_hdmi_clk/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend periph_ports [list "axi_iic_gth_clk/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend periph_ports [list "v_mix/s_axi_CTRL" "clk_wiz/clk_300M" "rst_clk_wiz_300M/peripheral_aresetn"]
lappend periph_ports [list "axi_timer/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]

# Create interface connections
connect_bd_intf_net [get_bd_intf_pins axi_iic_hdmi_clk/IIC] [get_bd_intf_ports HDMI_CLK_IIC]
connect_bd_intf_net [get_bd_intf_pins axi_uartlite/UART] [get_bd_intf_ports RS232_UART]
connect_bd_intf_net [get_bd_intf_pins tx_video_axis_reg_slice/M_AXIS] [get_bd_intf_pins v_hdmi_txss1/VIDEO_IN]
connect_bd_intf_net [get_bd_intf_pins v_hdmi_txss1/DDC_OUT] [get_bd_intf_ports TX_DDC_OUT]
# (v_mix master s_axis_video is now driven by bg_frmbuf_rd — connected at its creation below)
connect_bd_intf_net [get_bd_intf_pins v_mix/m_axis_video] [get_bd_intf_pins tx_video_axis_reg_slice/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins v_hdmi_phy1_0/vid_phy_status_sb_tx] [get_bd_intf_pins v_hdmi_txss1/SB_STATUS_IN]
connect_bd_intf_net [get_bd_intf_ports GTH_CLK_IIC] [get_bd_intf_pins axi_iic_gth_clk/IIC]
connect_bd_intf_net [get_bd_intf_pins v_hdmi_txss1/LINK_DATA0_OUT] [get_bd_intf_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_ch0]
connect_bd_intf_net [get_bd_intf_pins v_hdmi_txss1/LINK_DATA1_OUT] [get_bd_intf_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_ch1]
connect_bd_intf_net [get_bd_intf_pins v_hdmi_txss1/LINK_DATA2_OUT] [get_bd_intf_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_ch2]
# TMDS-only uses 3 link channels; LINK_DATA3_OUT (4th FRL lane) is removed at C_MAX_FRL_RATE=0.
if {[llength [get_bd_intf_pins -quiet v_hdmi_txss1/LINK_DATA3_OUT]]} {
  connect_bd_intf_net [get_bd_intf_pins v_hdmi_txss1/LINK_DATA3_OUT] [get_bd_intf_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_ch3]
}
connect_bd_intf_net [get_bd_intf_ports DRU_CLK_IN] [get_bd_intf_pins util_ds_buf_1/CLK_IN_D]

# Create port connections
connect_bd_net [get_bd_ports IDT_8T49N241_LOL_IN] [get_bd_ports LED2] [get_bd_pins v_hdmi_phy1_0/tx_refclk_rdy]
connect_bd_net [get_bd_ports TX_HPD_IN] [get_bd_pins v_hdmi_txss1/hpd]
connect_bd_net [get_bd_ports TX_REFCLK_N_IN] [get_bd_pins v_hdmi_phy1_0/mgtrefclk1_pad_n_in]
connect_bd_net [get_bd_ports TX_REFCLK_P_IN] [get_bd_pins v_hdmi_phy1_0/mgtrefclk1_pad_p_in]
connect_bd_net [get_bd_pins clk_wiz/clk_300M] [get_bd_pins tx_video_axis_reg_slice/aclk] [get_bd_pins v_hdmi_txss1/s_axis_video_aclk] [get_bd_pins rst_clk_wiz_300M/slowest_sync_clk]
connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins v_hdmi_txss1/s_axi_cpu_aclk] [get_bd_pins v_hdmi_phy1_0/vid_phy_sb_aclk] [get_bd_pins v_hdmi_phy1_0/vid_phy_axi4lite_aclk] [get_bd_pins v_hdmi_phy1_0/drpclk] [get_bd_pins axi_uartlite/s_axi_aclk] [get_bd_pins axi_iic_hdmi_clk/s_axi_aclk] [get_bd_pins axi_iic_gth_clk/s_axi_aclk] [get_bd_pins microblaze_0/Clk] [get_bd_pins rst_clk_wiz_100M/slowest_sync_clk]
# TMDS-only (C_HDMI_MODE=0, C_MAX_FRL_RATE=0): the FRL clock input is removed from v_hdmi_txss1.
# Only wire clk_450M -> frl_clk if the pin still exists, so this BD stays valid for FRL or TMDS builds.
if {[llength [get_bd_pins -quiet v_hdmi_txss1/frl_clk]]} {
  connect_bd_net [get_bd_pins clk_wiz/clk_450M] [get_bd_pins v_hdmi_txss1/frl_clk]
}
connect_bd_net [get_bd_pins clk_wiz/locked] [get_bd_pins rst_clk_wiz_100M/aux_reset_in] [get_bd_pins rst_clk_wiz_100M/dcm_locked] [get_bd_pins rst_clk_wiz_300M/aux_reset_in] [get_bd_pins rst_clk_wiz_300M/dcm_locked]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_ports LED1] [get_bd_ports IDT_8T49N241_RST_OUT] [get_bd_pins v_hdmi_txss1/s_axi_cpu_aresetn] [get_bd_pins v_hdmi_phy1_0/vid_phy_sb_aresetn] [get_bd_pins v_hdmi_phy1_0/vid_phy_axi4lite_aresetn] [get_bd_pins axi_uartlite/s_axi_aresetn] [get_bd_pins axi_iic_hdmi_clk/s_axi_aresetn] [get_bd_pins axi_iic_gth_clk/s_axi_aresetn]
connect_bd_net [get_bd_pins rst_clk_wiz_300M/peripheral_aresetn] [get_bd_pins tx_video_axis_reg_slice/aresetn] [get_bd_pins v_hdmi_txss1/s_axis_video_aresetn]
connect_bd_net [get_bd_pins invert_rst/Res] [get_bd_pins rst_clk_wiz_300M/ext_reset_in]
connect_bd_net [get_bd_pins v_hdmi_txss1/locked] [get_bd_ports LED0]
# NOTE(iterate): video_cke_in + audio(acr/aclk/aresetn) tied to vcc here (reference sourced them
# from an RX-passthrough FIFO + audio_ss we don't have). BUFG_GT_CE tied high.
connect_bd_net [get_bd_pins vcc_const/dout] [get_bd_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_aresetn] [get_bd_pins v_hdmi_txss1/s_axis_audio_aclk] [get_bd_pins v_hdmi_txss1/s_axis_audio_aresetn] [get_bd_pins util_ds_buf_0/BUFG_GT_CE]
# video_cke_in is present only in FRL mode (removed at C_MAX_FRL_RATE=0); tie to vcc if it exists.
if {[llength [get_bd_pins -quiet v_hdmi_txss1/video_cke_in]]} {
  connect_bd_net [get_bd_pins vcc_const/dout] [get_bd_pins v_hdmi_txss1/video_cke_in]
}
connect_bd_net [get_bd_pins v_hdmi_phy1_0/phy_txn_out] [get_bd_ports HDMI_TX_DAT_N_OUT]
connect_bd_net [get_bd_pins v_hdmi_phy1_0/phy_txp_out] [get_bd_ports HDMI_TX_DAT_P_OUT]
connect_bd_net [get_bd_pins v_hdmi_phy1_0/tx_video_clk] [get_bd_pins v_hdmi_txss1/video_clk]
connect_bd_net [get_bd_pins v_hdmi_phy1_0/txoutclk] [get_bd_pins v_hdmi_txss1/link_clk] [get_bd_pins v_hdmi_phy1_0/vid_phy_tx_axi4s_aclk]
connect_bd_net [get_bd_pins util_ds_buf_1/IBUF_OUT] [get_bd_pins v_hdmi_phy1_0/gtsouthrefclk1_in] [get_bd_pins v_hdmi_phy1_0/gtsouthrefclk10_in] [get_bd_pins v_hdmi_phy1_0/gtsouthrefclk11_in]
connect_bd_net [get_bd_pins util_ds_buf_1/IBUF_DS_ODIV2] [get_bd_pins util_ds_buf_0/BUFG_GT_I]
connect_bd_net [get_bd_pins util_ds_buf_0/BUFG_GT_O] [get_bd_pins v_hdmi_phy1_0/gtsouthrefclk1_odiv2_in]

# Interrupts
lappend intr_list "axi_timer/interrupt"
lappend intr_list "v_hdmi_phy1_0/irq"
lappend intr_list "v_hdmi_txss1/irq"
lappend intr_list "v_mix/interrupt"

##### Add the MIPI pipes #####

# Procedure for creating a MIPI pipe for one camera
proc create_mipi_pipe { index loc_dict } {
  set hier_obj [create_bd_cell -type hier mipi_$index]
  current_bd_instance $hier_obj
  global pipe_samples_pc
  global disp_samples_pc
  global target
  global max_cols
  global max_rows
  
  # Create pins of the block
  create_bd_pin -dir I dphy_clk_200M
  create_bd_pin -dir I s_axi_lite_aclk
  create_bd_pin -dir I aresetn
  create_bd_pin -dir I video_aclk
  create_bd_pin -dir I video_aresetn
  create_bd_pin -dir O -type intr mipi_sub_irq
  create_bd_pin -dir O -type intr frmbufwr_irq
  create_bd_pin -dir O -type intr frmbufrd_irq
  create_bd_pin -dir O -type intr iic2intc_irpt
  create_bd_pin -dir O GPIO0
  create_bd_pin -dir O GPIO1
  
  # Create the interfaces of the block
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_CTRL
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_VIDEO
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_MM
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_VIDEO
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:mipi_phy_rtl:1.0 MIPI_PHY_IF
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 IIC
  
  # Add and configure AXI GPIO
  set axi_gpio [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_0]
  set_property -dict [list CONFIG.C_GPIO_WIDTH {32} CONFIG.C_ALL_OUTPUTS {1} CONFIG.C_DOUT_DEFAULT {0x00000001} ] $axi_gpio
  
  # Add and configure the MIPI Subsystem IP
  set clk_pin [dict get $loc_dict clk pin]
  set clk_pin_name [dict get $loc_dict clk pin_name]
  set data0_pin [dict get $loc_dict data0 pin]
  set data0_pin_name [dict get $loc_dict data0 pin_name]
  set data1_pin [dict get $loc_dict data1 pin]
  set data1_pin_name [dict get $loc_dict data1 pin_name]
  set bank [dict get $loc_dict bank]

  # Clean reference personality for BOTH interfaces: 2-lane RAW10 @ 420 Mbps,
  # HS_SETTLE 158, subset converter takes the top 8 of 10 bits. No register
  # interface, no 4-lane - this is the known-good receiver the reference design
  # uses (4-lane proved impossible on this board: D3 lands on an unusable _N8
  # D-PHY bitslice on all connectors). Minimal build to get a clean 2-lane
  # receiver whose lane 1 works.
  set mipi_num_lanes   {2}
  set mipi_pxl_format  {RAW10}
  set mipi_line_rate   {420}
  set mipi_hs_settle   {158}
  set mipi_tdata_remap {tdata[9:2]}
  # D-PHY register interface ENABLED on BOTH capture pipelines so HS_SETTLE and
  # the D-PHY lane status can be tuned/inspected live over JTAG on either MIPI
  # link (kept active as a standing debug capability). Enabling it makes each
  # CSI-2 RX subsystem 8 KB (D-PHY regs at base+0x1000); addresses are assigned
  # symmetrically below so the two 8 KB blocks never collide.
  set mipi_reg_if {true}

  set mipi_csi2_rx_subsyst [ create_bd_cell -type ip -vlnv xilinx.com:ip:mipi_csi2_rx_subsystem mipi_csi2_rx_subsyst_0 ]
  # Base config (lanes 0/1 always present).
  set mipi_cfg [list \
    CONFIG.SupportLevel {1} \
    CONFIG.CMN_NUM_LANES $mipi_num_lanes \
    CONFIG.CMN_PXL_FORMAT $mipi_pxl_format \
    CONFIG.C_DPHY_LANES $mipi_num_lanes \
    CONFIG.CMN_NUM_PIXELS $pipe_samples_pc \
    CONFIG.C_EN_CSI_V2_0 {true} \
    CONFIG.DPY_EN_REG_IF $mipi_reg_if \
    CONFIG.C_HS_LINE_RATE $mipi_line_rate \
    CONFIG.C_HS_SETTLE_NS $mipi_hs_settle \
    CONFIG.DPY_LINE_RATE $mipi_line_rate \
    CONFIG.CLK_LANE_IO_LOC $clk_pin \
    CONFIG.CLK_LANE_IO_LOC_NAME $clk_pin_name \
    CONFIG.DATA_LANE0_IO_LOC $data0_pin \
    CONFIG.DATA_LANE0_IO_LOC_NAME $data0_pin_name \
    CONFIG.DATA_LANE1_IO_LOC $data1_pin \
    CONFIG.DATA_LANE1_IO_LOC_NAME $data1_pin_name \
    CONFIG.HP_IO_BANK_SELECTION $bank \
  ]
  # Lanes 2/3 only on the 4-lane interface.
  if { $mipi_num_lanes == 4 } {
    lappend mipi_cfg \
      CONFIG.DATA_LANE2_IO_LOC $data2_pin \
      CONFIG.DATA_LANE2_IO_LOC_NAME $data2_pin_name \
      CONFIG.DATA_LANE3_IO_LOC $data3_pin \
      CONFIG.DATA_LANE3_IO_LOC_NAME $data3_pin_name
  }
  set_property -dict $mipi_cfg $mipi_csi2_rx_subsyst
 
  # Add and configure the AXI SmartConnect (control)
  # NOTE: the legacy axi_interconnect IP is not supported on the SCU200 part
  # (Spartan UltraScale+); AXI SmartConnect is the supported replacement.
  set axi_int_ctrl [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axi_int_ctrl ]
  set_property -dict [list \
  CONFIG.NUM_SI {1} \
  CONFIG.NUM_MI {3} \
  ] $axi_int_ctrl
  
  # Add and configure the AXI SmartConnect (video control) - see note above.
  set axi_int_video [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axi_int_video ]
  set_property -dict [list \
  CONFIG.NUM_SI {1} \
  CONFIG.NUM_MI {2} \
  ] $axi_int_video
  
  # Add and configure the AXIS Subset Converter — MONOCHROME pipeline.
  # The RX packs each RAW10 pixel into a 2-byte container; extract the top 8 bits
  # ({tdata[9:2]}) as an 8-bit luma sample, then REPLICATE it into R=G=B so the
  # downstream frame buffer + HDMI display path stay RGB8 unchanged and render the
  # raw sensor luma as true grayscale (no demosaic / gamma / scaler needed).
  # (pipe_samples_pc is 1 for this variant; the >1 branch is kept for completeness.)
  if { $pipe_samples_pc == 1 } {
    set subset_conv_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter subset_conv_0 ]
    set_property -dict [ list \
     CONFIG.M_TDATA_NUM_BYTES {3} \
     CONFIG.S_TDATA_NUM_BYTES {2} \
     CONFIG.TDATA_REMAP {tdata[9:2],tdata[9:2],tdata[9:2]} \
    ] $subset_conv_0
  } else {
    set subset_conv_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter subset_conv_0 ]
    set_property -dict [ list \
     CONFIG.M_TDATA_NUM_BYTES {6} \
     CONFIG.S_TDATA_NUM_BYTES {3} \
     CONFIG.TDATA_REMAP {tdata[19:12],tdata[19:12],tdata[19:12],tdata[9:2],tdata[9:2],tdata[9:2]} \
    ] $subset_conv_0
  }
  
  # MONOCHROME pipeline: demosaic, v_gamma_lut and v_proc (scaler) REMOVED.
  # The replicated-luma RGB stream from subset_conv_0 feeds v_frmbuf_wr directly.

  # Add and configure the Video Frame Buffer Write
  set v_frmbuf_wr [create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr v_frmbuf_wr]
  set_property -dict [list \
   CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH {128} \
   CONFIG.SAMPLES_PER_CLOCK $pipe_samples_pc \
   CONFIG.AXIMM_DATA_WIDTH {128} \
   CONFIG.MAX_COLS $max_cols \
   CONFIG.MAX_ROWS $max_rows \
  ] $v_frmbuf_wr
  
  # Add and configure the Video Frame Buffer Read
  set v_frmbuf_rd [create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd v_frmbuf_rd]
  set_property -dict [list \
   CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH {128} \
   CONFIG.SAMPLES_PER_CLOCK $disp_samples_pc \
   CONFIG.AXIMM_DATA_WIDTH {128} \
   CONFIG.MAX_COLS $max_cols \
   CONFIG.MAX_ROWS $max_rows \
  ] $v_frmbuf_rd

  # AXI SmartConnect for the Frame Buffer MM interfaces - see note above.
  set axi_int_mem [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axi_int_mem ]
  set_property -dict [list \
    CONFIG.NUM_SI {2} \
    CONFIG.NUM_MI {1} \
  ] [get_bd_cells axi_int_mem]
  
  connect_bd_intf_net [get_bd_intf_pins v_frmbuf_wr/m_axi_mm_video] [get_bd_intf_pins axi_int_mem/S00_AXI]
  connect_bd_intf_net [get_bd_intf_pins v_frmbuf_rd/m_axi_mm_video] [get_bd_intf_pins axi_int_mem/S01_AXI]
  connect_bd_intf_net [get_bd_intf_pins axi_int_mem/M00_AXI] [get_bd_intf_pins M_AXI_MM]
  
  # Slice for Camera GPIO0 signal
  set resetn_index 0
  set cam_gpio0 [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 cam_gpio0 ]
  set_property -dict [ list \
  CONFIG.DIN_WIDTH {32} \
  CONFIG.DIN_TO $resetn_index \
  CONFIG.DIN_FROM $resetn_index \
  CONFIG.DOUT_WIDTH {1} \
  ] $cam_gpio0

  connect_bd_net [get_bd_pins cam_gpio0/Dout] [get_bd_pins GPIO0]
  connect_bd_net [get_bd_pins axi_gpio_0/gpio_io_o] [get_bd_pins cam_gpio0/Din]

  # Slice for Camera GPIO1 signal
  set resetn_index 1
  set cam_gpio1 [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 cam_gpio1 ]
  set_property -dict [ list \
  CONFIG.DIN_WIDTH {32} \
  CONFIG.DIN_TO $resetn_index \
  CONFIG.DIN_FROM $resetn_index \
  CONFIG.DOUT_WIDTH {1} \
  ] $cam_gpio1

  connect_bd_net [get_bd_pins cam_gpio1/Dout] [get_bd_pins GPIO1]
  connect_bd_net [get_bd_pins axi_gpio_0/gpio_io_o] [get_bd_pins cam_gpio1/Din]

  # MONOCHROME pipeline: reset slices for demosaic / v_proc / gamma REMOVED
  # (GPIO bits 2,3,4 are now unused; frmbuf resets keep bits 5,6).

  # Slice for Frmbuf RD reset signal
  set resetn_index 5
  set reset_frmbuf_rd [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 reset_frmbuf_rd ]
  set_property -dict [ list \
  CONFIG.DIN_WIDTH {32} \
  CONFIG.DIN_TO $resetn_index \
  CONFIG.DIN_FROM $resetn_index \
  CONFIG.DOUT_WIDTH {1} \
  ] $reset_frmbuf_rd

  connect_bd_net -net reset_frmbuf_rd_Dout [get_bd_pins reset_frmbuf_rd/Dout] [get_bd_pins v_frmbuf_rd/ap_rst_n]
  connect_bd_net [get_bd_pins axi_gpio_0/gpio_io_o] [get_bd_pins reset_frmbuf_rd/Din]

  # Slice for Frmbuf WR reset signal
  set resetn_index 6
  set reset_frmbuf_wr [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice:1.0 reset_frmbuf_wr ]
  set_property -dict [ list \
  CONFIG.DIN_WIDTH {32} \
  CONFIG.DIN_TO $resetn_index \
  CONFIG.DIN_FROM $resetn_index \
  CONFIG.DOUT_WIDTH {1} \
  ] $reset_frmbuf_wr

  connect_bd_net -net reset_frmbuf_wr_Dout [get_bd_pins reset_frmbuf_wr/Dout] [get_bd_pins v_frmbuf_wr/ap_rst_n]
  connect_bd_net [get_bd_pins axi_gpio_0/gpio_io_o] [get_bd_pins reset_frmbuf_wr/Din]

  # Reg slice for AXIS video output
  set video_axis_reg_slice [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice video_axis_reg_slice ]

  # Add and configure AXI IIC
  set axi_iic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic axi_iic_0]
  
  # Connect the 200M D-PHY clock
  connect_bd_net [get_bd_pins dphy_clk_200M] [get_bd_pins mipi_csi2_rx_subsyst_0/dphy_clk_200M]
  # Connect the video clock
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins mipi_csi2_rx_subsyst_0/video_aclk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins subset_conv_0/aclk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins v_frmbuf_wr/ap_clk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins v_frmbuf_rd/ap_clk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins axi_int_video/aclk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins axi_int_mem/aclk]
  connect_bd_net [get_bd_pins video_aclk] [get_bd_pins video_axis_reg_slice/aclk]
  # Connect the 100M AXI-Lite clock
  connect_bd_net [get_bd_pins s_axi_lite_aclk] [get_bd_pins axi_int_ctrl/aclk]
  connect_bd_net [get_bd_pins s_axi_lite_aclk] [get_bd_pins mipi_csi2_rx_subsyst_0/lite_aclk]
  connect_bd_net [get_bd_pins s_axi_lite_aclk] [get_bd_pins axi_iic_0/s_axi_aclk]
  connect_bd_net [get_bd_pins s_axi_lite_aclk] [get_bd_pins axi_gpio_0/s_axi_aclk]
  # Connect the video resets
  connect_bd_net [get_bd_pins video_aresetn] [get_bd_pins subset_conv_0/aresetn]
  connect_bd_net [get_bd_pins video_aresetn] [get_bd_pins axi_int_video/aresetn] -boundary_type upper
  connect_bd_net [get_bd_pins video_aresetn] [get_bd_pins mipi_csi2_rx_subsyst_0/video_aresetn]
  connect_bd_net [get_bd_pins video_aresetn] [get_bd_pins axi_int_mem/aresetn]
  connect_bd_net [get_bd_pins video_aresetn] [get_bd_pins video_axis_reg_slice/aresetn]
  # Connect the AXI-Lite resets
  connect_bd_net [get_bd_pins aresetn] [get_bd_pins axi_int_ctrl/aresetn] -boundary_type upper
  connect_bd_net [get_bd_pins aresetn] [get_bd_pins mipi_csi2_rx_subsyst_0/lite_aresetn]
  connect_bd_net [get_bd_pins aresetn] [get_bd_pins axi_iic_0/s_axi_aresetn]
  connect_bd_net [get_bd_pins aresetn] [get_bd_pins axi_gpio_0/s_axi_aresetn]
  # Connect AXI Lite CTRL interfaces
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins S_AXI_CTRL] [get_bd_intf_pins axi_int_ctrl/S00_AXI]
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins axi_int_ctrl/M00_AXI] [get_bd_intf_pins mipi_csi2_rx_subsyst_0/csirxss_s_axi]
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins axi_int_ctrl/M01_AXI] [get_bd_intf_pins axi_iic_0/S_AXI]
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins axi_int_ctrl/M02_AXI] [get_bd_intf_pins axi_gpio_0/S_AXI]
  # Connect AXI Lite VIDEO interfaces
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins S_AXI_VIDEO] [get_bd_intf_pins axi_int_video/S00_AXI]
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins axi_int_video/M00_AXI] [get_bd_intf_pins v_frmbuf_wr/s_axi_CTRL]
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins axi_int_video/M01_AXI] [get_bd_intf_pins v_frmbuf_rd/s_axi_CTRL]
  # Connect the AXI Streaming interfaces — MONOCHROME: mipi -> subset_conv -> frmbuf_wr
  connect_bd_intf_net [get_bd_intf_pins mipi_csi2_rx_subsyst_0/video_out] [get_bd_intf_pins subset_conv_0/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins subset_conv_0/M_AXIS] [get_bd_intf_pins v_frmbuf_wr/s_axis_video]
  connect_bd_intf_net [get_bd_intf_pins v_frmbuf_rd/m_axis_video] [get_bd_intf_pins video_axis_reg_slice/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins video_axis_reg_slice/M_AXIS] [get_bd_intf_pins M_AXIS_VIDEO]
  # Connect the MIPI D-PHY interface
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins MIPI_PHY_IF] [get_bd_intf_pins mipi_csi2_rx_subsyst_0/mipi_phy_if]
  # Connect the I2C interface
  connect_bd_intf_net [get_bd_intf_pins IIC] [get_bd_intf_pins axi_iic_0/IIC]
  # Connect interrupts
  connect_bd_net [get_bd_pins mipi_sub_irq] [get_bd_pins mipi_csi2_rx_subsyst_0/csirxss_csi_irq]
  connect_bd_net [get_bd_pins frmbufwr_irq] [get_bd_pins v_frmbuf_wr/interrupt]
  connect_bd_net [get_bd_pins frmbufrd_irq] [get_bd_pins v_frmbuf_rd/interrupt]
  connect_bd_net [get_bd_pins iic2intc_irpt] [get_bd_pins axi_iic_0/iic2intc_irpt]
  
  current_bd_instance \
}

# Add constant for the CAM1 and CAM3 CLK_SEL pin (01b for UltraZed-EV Carrier + AUBoard and 00b for Genesys ZU, 10b for all other boards)
set clk_sel [create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconstant:1.0 clk_sel]
set_property -dict [list CONFIG.CONST_WIDTH {2}] $clk_sel
if { $target == "auboard" } {
  set_property -dict [list CONFIG.CONST_VAL {0x01}] $clk_sel
} else {
  set_property -dict [list CONFIG.CONST_VAL {0x02}] $clk_sel
}
create_bd_port -dir O clk_sel
connect_bd_net [get_bd_ports clk_sel] [get_bd_pins clk_sel/dout]

# Add and configure GPIO for the reserved GPIOs
set rsvd_gpio [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio rsvd_gpio]
set_property -dict [list CONFIG.C_GPIO_WIDTH {10} CONFIG.C_ALL_OUTPUTS {1} CONFIG.C_DOUT_DEFAULT {0x00000030} ] $rsvd_gpio
connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins rsvd_gpio/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins rsvd_gpio/s_axi_aresetn]
lappend periph_ports [list "rsvd_gpio/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gpio_rtl:1.0 rsvd_gpio
connect_bd_intf_net [get_bd_intf_pins rsvd_gpio/GPIO] [get_bd_intf_ports rsvd_gpio]

# Create AXI GPIO to drive the video port IP resets
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio main_resetn_gpio
set_property CONFIG.C_GPIO_WIDTH {32} [get_bd_cells main_resetn_gpio]
connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins main_resetn_gpio/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins main_resetn_gpio/s_axi_aresetn]
connect_bd_net [get_bd_pins main_resetn_gpio/gpio_io_o] [get_bd_pins reset_v_tpg/Din]
connect_bd_net [get_bd_pins main_resetn_gpio/gpio_io_o] [get_bd_pins reset_v_mix/Din]
lappend periph_ports [list "main_resetn_gpio/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]

# Reconfigure the DDR AXI SmartConnect to accommodate the Frame Buffers + bg reader + mbdpu03
set smartcon_ports [expr {$num_cams+4}]
set_property -dict [list \
  CONFIG.NUM_SI $smartcon_ports \
  CONFIG.NUM_CLKS {3} \
] [get_bd_cells axi_smc]
connect_bd_net [get_bd_pins clk_wiz/clk_300M] [get_bd_pins axi_smc/aclk2]

# Add the MIPI pipes
set smartcon_index 2
set vmix_index 1
foreach i $cams {
  # Create the MIPI pipe block
  create_mipi_pipe $i [dict get $mipi_loc_dict $target $i]
  # Externalize all of the strobe propagation pins
  set strobe_pins [get_bd_pins mipi_$i/mipi_csi2_rx_subsyst_0/bg*_nc]
  foreach strobe $strobe_pins {
    set strobe_pin_name [file tail $strobe]
    create_bd_port -dir I mipi_${i}_$strobe_pin_name
    connect_bd_net [get_bd_ports mipi_${i}_$strobe_pin_name] [get_bd_pins $strobe]
  }
  # Connect clocks
  connect_bd_net [get_bd_pins clk_wiz/clk_200M] [get_bd_pins mipi_$i/dphy_clk_200M]
  connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins mipi_$i/s_axi_lite_aclk]
  connect_bd_net [get_bd_pins $cam_clk] [get_bd_pins mipi_$i/video_aclk]
  # Connect resets
  connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins mipi_$i/aresetn]
  connect_bd_net [get_bd_pins $cam_rst/peripheral_aresetn] [get_bd_pins mipi_$i/video_aresetn]
  # Add interrupts to the interrupt list to be connected later
  lappend intr_list "mipi_$i/mipi_sub_irq"
  lappend intr_list "mipi_$i/frmbufwr_irq"
  lappend intr_list "mipi_$i/frmbufrd_irq"
  lappend intr_list "mipi_$i/iic2intc_irpt"
  
  # AXI Lite interfaces to be connected later
  lappend periph_ports [list "mipi_$i/S_AXI_CTRL" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
  lappend periph_ports [list "mipi_$i/S_AXI_VIDEO" $cam_clk $cam_rst/peripheral_aresetn]
  # Connect the MIPI D-Phy interface
  create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:mipi_phy_rtl:1.0 mipi_phy_if_$i
  connect_bd_intf_net [get_bd_intf_ports mipi_phy_if_$i] -boundary_type upper [get_bd_intf_pins mipi_$i/MIPI_PHY_IF]
  # Connect the I2C interface
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 iic_$i
  connect_bd_intf_net [get_bd_intf_ports iic_$i] [get_bd_intf_pins mipi_$i/IIC]
  # Connect GPIOs
  create_bd_port -dir O mipi_${i}_gpio0
  create_bd_port -dir O mipi_${i}_gpio1
  connect_bd_net [get_bd_pins mipi_$i/GPIO0] [get_bd_ports mipi_${i}_gpio0]
  connect_bd_net [get_bd_pins mipi_$i/GPIO1] [get_bd_ports mipi_${i}_gpio1]
  # Connect the AXI MM interfaces of the Frame Buffers
  connect_bd_intf_net -boundary_type upper [get_bd_intf_pins mipi_$i/M_AXI_MM] [get_bd_intf_pins axi_smc/S0${smartcon_index}_AXI]
  # Connect the video AXIS interface
  connect_bd_intf_net [get_bd_intf_pins mipi_$i/M_AXIS_VIDEO] [get_bd_intf_pins v_mix/s_axis_video${vmix_index}]
  # Increment the smartconnector and v_mixer index
  set smartcon_index [expr {$smartcon_index+1}]
  set vmix_index [expr {$vmix_index+1}]
}

#########################################################
# bg_frmbuf_rd - Option A background image reader (opt-4, replaces v_tpg cross-hatch)
# Streams a static 1920x1080 image from DDR (software loads it once at boot) into the
# v_mix MASTER layer. ap_clk = clk_300M, ap_rst_n reuses reset_v_tpg (gpio bit 0),
# and its DDR read master takes the next free axi_smc SI (300 MHz domain).
#########################################################
set bg_frmbuf_rd [create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd bg_frmbuf_rd]
set_property -dict [list \
 CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH {128} \
 CONFIG.SAMPLES_PER_CLOCK $disp_samples_pc \
 CONFIG.AXIMM_DATA_WIDTH {128} \
 CONFIG.MAX_COLS $max_cols \
 CONFIG.MAX_ROWS $max_rows \
] $bg_frmbuf_rd
connect_bd_net [get_bd_pins clk_wiz/clk_300M] [get_bd_pins bg_frmbuf_rd/ap_clk]
connect_bd_net [get_bd_pins reset_v_tpg/Dout] [get_bd_pins bg_frmbuf_rd/ap_rst_n]
connect_bd_intf_net [get_bd_intf_pins bg_frmbuf_rd/m_axis_video] [get_bd_intf_pins v_mix/s_axis_video]
connect_bd_intf_net [get_bd_intf_pins bg_frmbuf_rd/m_axi_mm_video] [get_bd_intf_pins axi_smc/S0${smartcon_index}_AXI]

#########################################################
# mbdpu03 - custom DPU (112x112x1 INT8 MNIST CNN + dx/dy digit-position head), wired to ddr4_0
# HLS core xilinx.com:hls:mbdpu03:1.0. It reads the camera frame buffer itself (crop/zoom/luma,
# the job mbpre did in demo01/02), so there is no mbpre in this design. Feature maps and FC
# weights sit in UltraRAM (run_hls.tcl patches the exported RAMs). Runs at 100 MHz.
# m_axi_gmem -> next free axi_smc SI (100 MHz), s_axi_control + interrupt via MB periph bus.
#########################################################
incr smartcon_index
set mbdpu03 [ create_bd_cell -type ip -vlnv xilinx.com:hls:mbdpu03:1.0 mbdpu03 ]
connect_bd_intf_net [get_bd_intf_pins mbdpu03/m_axi_gmem] [get_bd_intf_pins axi_smc/S0${smartcon_index}_AXI]
connect_bd_net [get_bd_pins clk_wiz/clk_100M] [get_bd_pins mbdpu03/ap_clk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins mbdpu03/ap_rst_n]
lappend periph_ports [list "mbdpu03/s_axi_control" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
lappend intr_list "mbdpu03/interrupt"

#########################################################
# Motor control — DC Motor 15 Click (DRV8874) on CLICK1, quad encoder tap on CLICK2
#########################################################
# DRV8874 is in PH/EN mode. The DC Motor 15 Click routes EN (speed) to the mikroBUS
# CS pin and PH (direction) to the mikroBUS PWM pin, so:
#   speed  = PWM  -> CLICK1_CS   (motor_pwm/pwm0)
#   dir    = GPIO -> CLICK1_PWM  (motor_gpio_o[0])
#   nSLEEP = GPIO -> CLICK1_RST  (motor_gpio_o[1])  (drive HIGH to enable driver)
# Click 3.3V rails are load-switched: MCLICK1/2_3V3_ON must be asserted.
# Encoder A/B are tapped from the unused CLICK2 socket (MISO/MOSI). All bank 66, 3.3 V.

# --- PWM speed: a dedicated axi_timer in PWM mode (both timers); pwm0 -> CLICK1_CS ---
set motor_pwm [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_timer motor_pwm ]
set_property -dict [list CONFIG.enable_timer2 {1}] $motor_pwm
connect_bd_net [get_bd_pins clk_wiz/clk_100M]                    [get_bd_pins motor_pwm/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins motor_pwm/s_axi_aresetn]
lappend periph_ports [list "motor_pwm/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
create_bd_port -dir O MOTOR_PWM
connect_bd_net [get_bd_pins motor_pwm/pwm0] [get_bd_ports MOTOR_PWM]

# --- Motor GPIO: ch1 = 4 outputs (DIR/nSLEEP/CLICK1_3V3_ON/CLICK2_3V3_ON),
#                 ch2 = 3 inputs  (nFAULT / ENC_A / ENC_B). Outputs default LOW =
#                 motor disabled + click power off at reset (safe). ---
set motor_gpio [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio motor_gpio ]
set_property -dict [list \
  CONFIG.C_IS_DUAL {1} \
  CONFIG.C_GPIO_WIDTH {4} \
  CONFIG.C_ALL_OUTPUTS {1} \
  CONFIG.C_GPIO2_WIDTH {3} \
  CONFIG.C_ALL_INPUTS_2 {1} \
  CONFIG.C_DOUT_DEFAULT {0x00000000} \
] $motor_gpio
connect_bd_net [get_bd_pins clk_wiz/clk_100M]                    [get_bd_pins motor_gpio/s_axi_aclk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins motor_gpio/s_axi_aresetn]
lappend periph_ports [list "motor_gpio/S_AXI" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]
create_bd_port -dir O -from 3 -to 0 MOTOR_GPIO_O
connect_bd_net [get_bd_pins motor_gpio/gpio_io_o] [get_bd_ports MOTOR_GPIO_O]
create_bd_port -dir I -from 2 -to 0 MOTOR_GPIO_I
connect_bd_net [get_bd_ports MOTOR_GPIO_I] [get_bd_pins motor_gpio/gpio2_io_i]

#########################################################
# disc_strobe — encoder quadrature decode + angle-triggered camera FSIN strobe
#########################################################
# Free-running HLS core: decodes ENC_A/ENC_B (forked from the CLICK2 encoder inputs,
# MOTOR_GPIO_I[1]=A, [2]=B), tracks the disc angle, pulses XVS -> OV9281 FSIN when the
# position crosses target_angle. AXI-Lite: cpr/target_angle/pulse_width/enable + RO
# position/rev_count.
set disc_strobe [ create_bd_cell -type ip -vlnv xilinx.com:hls:disc_strobe:1.0 disc_strobe ]
connect_bd_net [get_bd_pins clk_wiz/clk_100M]                    [get_bd_pins disc_strobe/ap_clk]
connect_bd_net [get_bd_pins rst_clk_wiz_100M/peripheral_aresetn] [get_bd_pins disc_strobe/ap_rst_n]

# Fork encoder A (bit 1) and B (bit 2) out of MOTOR_GPIO_I into the decoder.
set enc_a_slice [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice enc_a_slice ]
set_property -dict [list CONFIG.DIN_WIDTH {3} CONFIG.DIN_FROM {1} CONFIG.DIN_TO {1} CONFIG.DOUT_WIDTH {1}] $enc_a_slice
connect_bd_net [get_bd_ports MOTOR_GPIO_I]    [get_bd_pins enc_a_slice/Din]
connect_bd_net [get_bd_pins enc_a_slice/Dout] [get_bd_pins disc_strobe/enc_a]
set enc_b_slice [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice enc_b_slice ]
set_property -dict [list CONFIG.DIN_WIDTH {3} CONFIG.DIN_FROM {2} CONFIG.DIN_TO {2} CONFIG.DOUT_WIDTH {1}] $enc_b_slice
connect_bd_net [get_bd_ports MOTOR_GPIO_I]    [get_bd_pins enc_b_slice/Din]
connect_bd_net [get_bd_pins enc_b_slice/Dout] [get_bd_pins disc_strobe/enc_b]

# XVS strobe out -> new top-level port (constrained to CLICK2_CS = D25 -> OV9281 FSIN).
create_bd_port -dir O MOTOR_XVS
connect_bd_net [get_bd_pins disc_strobe/xvs] [get_bd_ports MOTOR_XVS]

lappend periph_ports [list "disc_strobe/s_axi_ctrl" "clk_wiz/clk_100M" "rst_clk_wiz_100M/peripheral_aresetn"]

# scu200_ai_demo03_fcapz: add the fcapz event-timeline capture (marker GPIO + ELA).
# The plain demo03 target never sets fcapz_en, so its design is unchanged.
if {[info exists fcapz_en] && $fcapz_en} {
  source [file join [file dirname [info script]] fcapz_evtcap_bd.tcl]
}

#########################################################
# AXI Interfaces and interrupts
#########################################################

# Add AXI Interconnect for the AXI Lite interfaces

proc create_axi_ic {label ports} {
  set axi_ip [get_bd_cells $label]
  # Initial port index
  set port_num [get_property CONFIG.NUM_MI $axi_ip]
  # Initial clk index
  set clk_num [get_property CONFIG.NUM_CLKS $axi_ip]
  # Set number of master ports
  set n_periph_ports [llength $ports]
  set n_periph_ports [expr {$n_periph_ports+$port_num}]
  set_property -dict [list CONFIG.NUM_MI $n_periph_ports] $axi_ip
  # Set number of clock inputs
  set n_clk_inputs [llength $ports]
  set n_clk_inputs [expr {$n_clk_inputs+$clk_num}]
  set_property -dict [list CONFIG.NUM_CLKS $n_clk_inputs] $axi_ip
  # Attach all of the ports, their clocks and resets
  foreach port $ports {
    set port_name [format "M%02d" $port_num]
    set port_label [lindex $port 0]
    connect_bd_intf_net -boundary_type upper [get_bd_intf_pins $label/${port_name}_AXI] [get_bd_intf_pins $port_label]
    set port_clk [lindex $port 1]
    connect_bd_net [get_bd_pins $port_clk] [get_bd_pins $label/aclk${clk_num}]
    set port_num [expr {$port_num+1}]
    set clk_num [expr {$clk_num+1}]
  }
}

# Connect peripheral AXI ports
create_axi_ic "microblaze_0_axi_periph" $periph_ports

# Connect the interrupts to AXI Intc(max 32 interrupts)
set n_interrupts [llength $intr_list]
set intr_concat [get_bd_cells "microblaze_0_xlconcat"]
set_property -dict [list CONFIG.NUM_PORTS $n_interrupts] $intr_concat
set intr_index 0
foreach intr $intr_list {
  connect_bd_net [get_bd_pins $intr] [get_bd_pins ${intr_concat}/In$intr_index]
  set intr_index [expr {$intr_index+1}]
}

# Assign addresses
assign_bd_address



# Restore current instance
current_bd_instance $oldCurInst

save_bd_design
