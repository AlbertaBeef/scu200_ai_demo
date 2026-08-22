#################################################################################
# Constraints for the SCU200 Evaluation Kit (XCSU200P)
#################################################################################
# No board file exists for the SCU200, so every pin is constrained here manually.
# Pin data from the SCU200 master XDC (rdf0732-scu200-xdc, SCU200_Eval_XDC_1V0).
#
# Sections:
#   1. DDR4 (16-bit, MT40A512M16, bank 46) + 300 MHz system clock
#   2. System reset
#   3. On-board RPi/MIPI cameras: cam0->MIPI1, cam2->MIPI2 (banks 45/43)
#   4. HDMI TX control + GT reference clock
#   5. UART + LEDs
#   6. Notes on parked / non-SCU200 ports
#################################################################################

#################################################################################
# 1. DDR4 (Micron MT40A512M16HA-083E, x16, bank 46)
#    PACKAGE_PIN only - the DDR4 IP generates IOSTANDARD/SLEW/DCI itself.
#################################################################################
# Address (A0..A13, then WE/CAS/RAS on A14/A15/A16)
set_property PACKAGE_PIN H25 [get_ports {ddr4_sdram_adr[0]}];   # DDR4_C0_A0
set_property PACKAGE_PIN A29 [get_ports {ddr4_sdram_adr[1]}];   # DDR4_C0_A1
set_property PACKAGE_PIN B28 [get_ports {ddr4_sdram_adr[2]}];   # DDR4_C0_A2
set_property PACKAGE_PIN A31 [get_ports {ddr4_sdram_adr[3]}];   # DDR4_C0_A3
set_property PACKAGE_PIN B31 [get_ports {ddr4_sdram_adr[4]}];   # DDR4_C0_A4
set_property PACKAGE_PIN A30 [get_ports {ddr4_sdram_adr[5]}];   # DDR4_C0_A5
set_property PACKAGE_PIN B29 [get_ports {ddr4_sdram_adr[6]}];   # DDR4_C0_A6
set_property PACKAGE_PIN C28 [get_ports {ddr4_sdram_adr[7]}];   # DDR4_C0_A7
set_property PACKAGE_PIN D28 [get_ports {ddr4_sdram_adr[8]}];   # DDR4_C0_A8
set_property PACKAGE_PIN C30 [get_ports {ddr4_sdram_adr[9]}];   # DDR4_C0_A9
set_property PACKAGE_PIN C29 [get_ports {ddr4_sdram_adr[10]}];  # DDR4_C0_A10
set_property PACKAGE_PIN B32 [get_ports {ddr4_sdram_adr[11]}];  # DDR4_C0_A11
set_property PACKAGE_PIN C31 [get_ports {ddr4_sdram_adr[12]}];  # DDR4_C0_A12
set_property PACKAGE_PIN J25 [get_ports {ddr4_sdram_adr[13]}];  # DDR4_C0_A13
set_property PACKAGE_PIN E29 [get_ports {ddr4_sdram_adr[14]}];  # DDR4_C0_WE_B
set_property PACKAGE_PIN E28 [get_ports {ddr4_sdram_adr[15]}];  # DDR4_C0_CAS_B
set_property PACKAGE_PIN J26 [get_ports {ddr4_sdram_adr[16]}];  # DDR4_C0_RAS_B
set_property PACKAGE_PIN K25 [get_ports {ddr4_sdram_act_n}];    # DDR4_C0_ACT_B
set_property PACKAGE_PIN D30 [get_ports {ddr4_sdram_ba[0]}];    # DDR4_C0_BA0
set_property PACKAGE_PIN E30 [get_ports {ddr4_sdram_ba[1]}];    # DDR4_C0_BA1
set_property PACKAGE_PIN F31 [get_ports {ddr4_sdram_bg}];       # DDR4_C0_BG0
set_property PACKAGE_PIN G27 [get_ports {ddr4_sdram_ck_c}];     # DDR4_C0_CK0_C
set_property PACKAGE_PIN H26 [get_ports {ddr4_sdram_ck_t}];     # DDR4_C0_CK0_T
set_property PACKAGE_PIN H32 [get_ports {ddr4_sdram_cke}];      # DDR4_C0_CKE0
set_property PACKAGE_PIN F29 [get_ports {ddr4_sdram_cs_n}];     # DDR4_C0_CS0_B
set_property PACKAGE_PIN G32 [get_ports {ddr4_sdram_odt}];      # DDR4_C0_ODT0
set_property PACKAGE_PIN L26 [get_ports {ddr4_sdram_reset_n}];  # DDR4_C0_RESET_B
# Data mask
set_property PACKAGE_PIN M26 [get_ports {ddr4_sdram_dm_n[0]}];  # DDR4_C0_DM_B0
set_property PACKAGE_PIN G31 [get_ports {ddr4_sdram_dm_n[1]}];  # DDR4_C0_DM_B1
# Data - byte 0
set_property PACKAGE_PIN L31 [get_ports {ddr4_sdram_dq[0]}];    # DDR4_C0_DQ0
set_property PACKAGE_PIN K31 [get_ports {ddr4_sdram_dq[1]}];    # DDR4_C0_DQ1
set_property PACKAGE_PIN L27 [get_ports {ddr4_sdram_dq[2]}];    # DDR4_C0_DQ2
set_property PACKAGE_PIN K27 [get_ports {ddr4_sdram_dq[3]}];    # DDR4_C0_DQ3
set_property PACKAGE_PIN K28 [get_ports {ddr4_sdram_dq[4]}];    # DDR4_C0_DQ4
set_property PACKAGE_PIN K29 [get_ports {ddr4_sdram_dq[5]}];    # DDR4_C0_DQ5
set_property PACKAGE_PIN K32 [get_ports {ddr4_sdram_dq[6]}];    # DDR4_C0_DQ6
set_property PACKAGE_PIN J32 [get_ports {ddr4_sdram_dq[7]}];    # DDR4_C0_DQ7
set_property PACKAGE_PIN J30 [get_ports {ddr4_sdram_dqs_c[0]}]; # DDR4_C0_DQS_C0
set_property PACKAGE_PIN K30 [get_ports {ddr4_sdram_dqs_t[0]}]; # DDR4_C0_DQS_T0
# Data - byte 1
set_property PACKAGE_PIN H30 [get_ports {ddr4_sdram_dq[8]}];    # DDR4_C0_DQ8
set_property PACKAGE_PIN H31 [get_ports {ddr4_sdram_dq[9]}];    # DDR4_C0_DQ9
set_property PACKAGE_PIN J27 [get_ports {ddr4_sdram_dq[10]}];   # DDR4_C0_DQ10
set_property PACKAGE_PIN J28 [get_ports {ddr4_sdram_dq[11]}];   # DDR4_C0_DQ11
set_property PACKAGE_PIN H29 [get_ports {ddr4_sdram_dq[12]}];   # DDR4_C0_DQ12
set_property PACKAGE_PIN G29 [get_ports {ddr4_sdram_dq[13]}];   # DDR4_C0_DQ13
set_property PACKAGE_PIN D32 [get_ports {ddr4_sdram_dq[14]}];   # DDR4_C0_DQ14
set_property PACKAGE_PIN C32 [get_ports {ddr4_sdram_dq[15]}];   # DDR4_C0_DQ15
set_property PACKAGE_PIN E32 [get_ports {ddr4_sdram_dqs_c[1]}]; # DDR4_C0_DQS_C1
set_property PACKAGE_PIN E31 [get_ports {ddr4_sdram_dqs_t[1]}]; # DDR4_C0_DQS_T1
# 300 MHz DDR4 system clock (CLK95211_OUT1) - IOSTANDARD set by the DDR4 IP
set_property PACKAGE_PIN H28 [get_ports {system_clock_300mhz_clk_p}]; # CLK95211_OUT1_DDR4_SYSCLK_P
set_property PACKAGE_PIN G28 [get_ports {system_clock_300mhz_clk_n}]; # CLK95211_OUT1_DDR4_SYSCLK_N

#################################################################################
# 2. System reset (active-low CPU reset pushbutton, bank 66, 3.3V)
#################################################################################
set_property PACKAGE_PIN B24 [get_ports system_resetn];        # CPU_RESET_B (SW13, active-low)
set_property IOSTANDARD LVCMOS33 [get_ports system_resetn]
# CPU_RESET_B is an active-low pushbutton. Without a guaranteed pull-up it can
# float low, which holds clk_wiz/resetn AND (inverted) ddr4_0/sys_rst asserted ->
# no fabric clocks -> dead MicroBlaze. Force an internal pull-up so it idles high.
set_property PULLTYPE PULLUP [get_ports system_resetn]

#################################################################################
# 3. On-board RPi/MIPI cameras  (video: HPIO bank 45 1.2V; control: bank 43 3.3V)
#    Camera 0 -> connector MIPI1 ; Camera 2 -> connector MIPI2 (2 lanes each).
#    NOTE: the MIPI D-PHY pin LOCs are also set in the CSI-2 RX IP via
#    mipi_locs.tcl; the PACKAGE_PINs below match those.
#################################################################################
# --- Camera 0 -> MIPI1 : I2C + control GPIO (bank 43, 3.3V) ---
set_property PACKAGE_PIN AM28 [get_ports iic_0_scl_io];        # MIPI1_I2C_SCL
set_property PACKAGE_PIN AM29 [get_ports iic_0_sda_io];        # MIPI1_I2C_SDA
set_property IOSTANDARD LVCMOS33 [get_ports iic_0_*]
set_property SLEW SLOW [get_ports iic_0_*]
# The SCU200 has NO external I2C pull-ups on the MIPI camera buses (verified in the
# schematic: MIPI1_I2C_SCL/SDA appear only at the FPGA and at connector J31 - no
# resistors anywhere on the net). RPi camera modules don't provide pull-ups either
# (on a Pi, the Pi supplies them), so without these the bus floats and nothing can
# ever ACK. Enable the FPGA's internal weak pull-ups.
set_property PULLTYPE PULLUP [get_ports iic_0_scl_io]
set_property PULLTYPE PULLUP [get_ports iic_0_sda_io]
set_property PACKAGE_PIN AJ28 [get_ports {mipi_0_gpio0[0]}];   # MIPI1_PWR_EN
set_property PACKAGE_PIN AK28 [get_ports {mipi_0_gpio1[0]}];   # MIPI1_LED_EN_XCLK
set_property IOSTANDARD LVCMOS33 [get_ports {mipi_0_gpio0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mipi_0_gpio1[0]}]
# --- Camera 0 -> MIPI1 : MIPI D-PHY (bank 45, 1.2V) ---
set_property PACKAGE_PIN Y27 [get_ports {mipi_phy_if_0_clk_p}];     # MIPI1_CK_P
set_property PACKAGE_PIN Y28 [get_ports {mipi_phy_if_0_clk_n}];     # MIPI1_CK_N
set_property PACKAGE_PIN U26 [get_ports {mipi_phy_if_0_data_p[0]}]; # MIPI1_D0_P
set_property PACKAGE_PIN T26 [get_ports {mipi_phy_if_0_data_n[0]}]; # MIPI1_D0_N
set_property PACKAGE_PIN Y29 [get_ports {mipi_phy_if_0_data_p[1]}]; # MIPI1_D1_P
set_property PACKAGE_PIN Y30 [get_ports {mipi_phy_if_0_data_n[1]}]; # MIPI1_D1_N
# MIPI0/J31 is now 2-lane (personality swapped to MIPI2); D2/D3 not used here.
set_property IOSTANDARD MIPI_DPHY_DCI [get_ports mipi_phy_if_0_clk_p]
set_property IOSTANDARD MIPI_DPHY_DCI [get_ports mipi_phy_if_0_clk_n]
set_property IOSTANDARD MIPI_DPHY_DCI [get_ports mipi_phy_if_0_data_p[*]]
set_property IOSTANDARD MIPI_DPHY_DCI [get_ports mipi_phy_if_0_data_n[*]]
set_property DIFF_TERM_ADV TERM_100 [get_ports mipi_phy_if_0_clk_p]
set_property DIFF_TERM_ADV TERM_100 [get_ports mipi_phy_if_0_clk_n]
set_property DIFF_TERM_ADV TERM_100 [get_ports mipi_phy_if_0_data_p[*]]
set_property DIFF_TERM_ADV TERM_100 [get_ports mipi_phy_if_0_data_n[*]]


#################################################################################
# 4. HDMI TX  (control on bank 61 3.3V; GT reference clock on Quad 221)
#################################################################################
# HDMI TX source DDC (I2C to sink)
set_property PACKAGE_PIN AL6 [get_ports TX_DDC_OUT_scl_io];    # HDMI_TX_SRC_SCL
set_property PACKAGE_PIN AL8 [get_ports TX_DDC_OUT_sda_io];    # HDMI_TX_SRC_SDA
set_property IOSTANDARD LVCMOS33 [get_ports TX_DDC_OUT_*]
# HDMI TX hot-plug detect
set_property PACKAGE_PIN AM6 [get_ports TX_HPD_IN];           # HDMI_TX_SRC_HPD
set_property IOSTANDARD LVCMOS33 [get_ports TX_HPD_IN]
# HDMI TX enable. Board signal HDMI_TX_ENABLE_N is ACTIVE-LOW, so the BD drives
# TX_EN_OUT from gnd_const (0) to enable the on-board HDMI TX buffer.
set_property PACKAGE_PIN AE6 [get_ports {TX_EN_OUT[0]}];      # HDMI_TX_ENABLE_N
set_property IOSTANDARD LVCMOS33 [get_ports {TX_EN_OUT[0]}]
# 8T49N241 clock generator reset + loss-of-lock
set_property PACKAGE_PIN AK9 [get_ports {IDT_8T49N241_RST_OUT[0]}]; # HDMI_8T49N241_RST
set_property IOSTANDARD LVCMOS33 [get_ports {IDT_8T49N241_RST_OUT[0]}]
set_property PACKAGE_PIN AM7 [get_ports IDT_8T49N241_LOL_IN];      # HDMI_8T49N241_LOL_IN
set_property IOSTANDARD LVCMOS33 [get_ports IDT_8T49N241_LOL_IN]
# HDMI control I2C (reaches the 8T49N241 @0x6C, TX redriver @0x5E, EDID @0x50)
set_property PACKAGE_PIN AE9 [get_ports HDMI_CLK_IIC_scl_io];  # HDMI_CTL_VERSAL_SCL
set_property PACKAGE_PIN AD9 [get_ports HDMI_CLK_IIC_sda_io];  # HDMI_CTL_VERSAL_SDA
set_property IOSTANDARD LVCMOS33 [get_ports HDMI_CLK_IIC_*]

# --- HDMI GT transmit lanes: GTH Quad 221 (only the _P pin is constrained; the
#     GT infers the _N of each differential pair). 4 lanes = HDMI 2.1 FRL.
set_property PACKAGE_PIN AM4 [get_ports {HDMI_TX_DAT_P_OUT[0]}]; # PCIE_TX3_HDMI_TX0_P  (MGTHTXP0_221)
set_property PACKAGE_PIN AK4 [get_ports {HDMI_TX_DAT_P_OUT[1]}]; # PCIE_TX2_HDMI_TX1_P  (MGTHTXP1_221)
set_property PACKAGE_PIN AH4 [get_ports {HDMI_TX_DAT_P_OUT[2]}]; # PCIE_TX1_HDMI_TX2_P  (MGTHTXP2_221)
set_property PACKAGE_PIN AF4 [get_ports {HDMI_TX_DAT_P_OUT[3]}]; # PCIE_TX0_HDMI_TXCLK_P (MGTHTXP3_221)

# --- HDMI TX reference clock: 8T49N241 output -> MGTREFCLK1, GTH Quad 221.
#     Nominal 297 MHz (period 3.367). This is the programmable TMDS ref clock;
#     the 8T49N241 is retuned per resolution by software over HDMI_CLK_IIC.
set_property PACKAGE_PIN AB8 [get_ports TX_REFCLK_P_IN];       # HDMI_8T49N241_OUT_C_P (MGTREFCLK1P_221)
create_clock -period 3.367 -name tx_mgt_refclk [get_ports TX_REFCLK_P_IN]

# --- HDMI PHY DRU / FRL fixed reference clock: SiT95211 -> MGTREFCLK1, Quad 223.
#     400 MHz (period 2.500). Note this lives in a DIFFERENT quad than the TX
#     lanes (221), which is why the PHY takes it via gtsouthrefclk* and why
#     C_TX_FRL_REFCLK_SEL=5.
set_property PACKAGE_PIN Y8 [get_ports DRU_CLK_IN_clk_p];      # CLK95211_OUT9_B223_REFCLK1_P (MGTREFCLK1P_223)
create_clock -period 2.500 -name dru_refclk [get_ports DRU_CLK_IN_clk_p]

#################################################################################
# 5. UART + user LEDs  (bank 65, 1.8V)
#################################################################################
set_property PACKAGE_PIN E18 [get_ports RS232_UART_txd];      # DUT_UART1_TXD
set_property PACKAGE_PIN F18 [get_ports RS232_UART_rxd];      # DUT_UART1_RXD
set_property IOSTANDARD LVCMOS18 [get_ports RS232_UART_*]
set_property PACKAGE_PIN B18 [get_ports LED0];                # LED1
set_property IOSTANDARD LVCMOS18 [get_ports LED0]
set_property PACKAGE_PIN C18 [get_ports {LED1[0]}];          # LED2
set_property IOSTANDARD LVCMOS18 [get_ports {LED1[0]}]
set_property PACKAGE_PIN H16 [get_ports LED2];                # LED3
set_property IOSTANDARD LVCMOS18 [get_ports LED2]

#################################################################################
# 5b. Unused block-design ports parked on the FMC-LPC connector (bank 86, 1.2V)
#################################################################################
# These ports exist in the block design but have no on-board SCU200 destination.
# Vivado's UCIO-1 DRC rejects a bitstream with unconstrained I/O, so they are
# parked on unused FMC-LPC pins purely to satisfy that DRC (no mezzanine needed).
# NOTE: VCCO_86 (VR_VCCO_86_ADJ) must be 1.2V for LVCMOS12 to be correct.
set_property PACKAGE_PIN D1 [get_ports {clk_sel[0]}];  # FMC LA25_N
set_property PACKAGE_PIN E1 [get_ports {clk_sel[1]}];  # FMC LA25_P
set_property IOSTANDARD LVCMOS12 [get_ports {clk_sel[*]}]

set_property PACKAGE_PIN D4  [get_ports {rsvd_gpio_tri_o[0]}]; # FMC LA04_P
set_property PACKAGE_PIN C4  [get_ports {rsvd_gpio_tri_o[1]}]; # FMC LA04_N
set_property PACKAGE_PIN A8  [get_ports {rsvd_gpio_tri_o[2]}]; # FMC LA07_P
set_property PACKAGE_PIN A7  [get_ports {rsvd_gpio_tri_o[3]}]; # FMC LA07_N
set_property PACKAGE_PIN E11 [get_ports {rsvd_gpio_tri_o[4]}]; # FMC LA13_P
set_property PACKAGE_PIN E10 [get_ports {rsvd_gpio_tri_o[5]}]; # FMC LA13_N
set_property PACKAGE_PIN B4  [get_ports {rsvd_gpio_tri_o[6]}]; # FMC LA27_P
set_property PACKAGE_PIN A4  [get_ports {rsvd_gpio_tri_o[7]}]; # FMC LA27_N
set_property PACKAGE_PIN E6  [get_ports {rsvd_gpio_tri_o[8]}]; # FMC LA29_P
set_property PACKAGE_PIN E5  [get_ports {rsvd_gpio_tri_o[9]}]; # FMC LA29_N
set_property IOSTANDARD LVCMOS12 [get_ports {rsvd_gpio_tri_o[*]}]

# GTH_CLK_IIC: a 2nd clock-generator I2C master present in the block design. The
# SCU200 has a single HDMI control bus (already driven by HDMI_CLK_IIC above), so
# this master has no on-board target; parked on a spare FMC pair (LA08).
set_property PACKAGE_PIN A10 [get_ports GTH_CLK_IIC_scl_io]; # FMC LA08_P
set_property PACKAGE_PIN A9  [get_ports GTH_CLK_IIC_sda_io]; # FMC LA08_N
set_property IOSTANDARD LVCMOS12 [get_ports GTH_CLK_IIC_*]
set_property SLEW SLOW [get_ports GTH_CLK_IIC_*]

#################################################################################
# 6. Notes on parked / non-SCU200 ports
#################################################################################
# A few block-design ports have no on-board SCU200 destination but must still be
# constrained (Vivado UCIO-1 DRC rejects unconstrained I/O). They are parked on
# the FMC-LPC connector in section 5b:
#   GTH_CLK_IIC (scl/sda) - a 2nd clock I2C in the BD; SCU200 has a single HDMI
#                           control bus (already used by HDMI_CLK_IIC).
#   clk_sel[1:0]          - FMC carrier clock mux; not on-board.
#   rsvd_gpio_tri_o[9:0]  - FMC carrier reserved header GPIO; not on-board.
# All top-level ports are constrained.
#
# set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
#################################################################################

#################################################################################
# Motor control (bank 66, 3.3 V) — DC Motor 15 Click (DRV8874) on CLICK1,
# quadrature encoder tapped from the CLICK2 socket. See bd_scu200_ai_demo01.tcl.
#   MOTOR_PWM      = CLICK1_PWM -> DRV8874 EN  (speed, PWM)   [HW-verified 2026-08-18:
#   MOTOR_GPIO_O[0]= CLICK1_CS  -> DRV8874 PH  (direction)     PWM pin = EN, CS = PH]
#   MOTOR_GPIO_O[1]= CLICK1_RST -> DRV8874 nSLEEP (HIGH = enable)
#   MOTOR_GPIO_O[2]= MCLICK1_3V3_ON (assert to power CLICK1 3.3 V)
#   MOTOR_GPIO_O[3]= MCLICK2_3V3_ON (assert to power CLICK2 3.3 V, encoder Vcc)
#   MOTOR_GPIO_I[0]= CLICK1_INT -> DRV8874 nFAULT
#   MOTOR_GPIO_I[1]= CLICK2_MISO -> encoder A
#   MOTOR_GPIO_I[2]= CLICK2_MOSI -> encoder B
#################################################################################
set_property -dict {PACKAGE_PIN J21 IOSTANDARD LVCMOS33} [get_ports MOTOR_PWM]         ;# CLICK1_PWM = EN  (speed) - swapped
set_property -dict {PACKAGE_PIN H21 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_O[0]}] ;# CLICK1_CS  = PH  (direction) - swapped
set_property -dict {PACKAGE_PIN J23 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_O[1]}]
set_property -dict {PACKAGE_PIN G24 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_O[2]}]
set_property -dict {PACKAGE_PIN D23 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_O[3]}]
set_property -dict {PACKAGE_PIN K21 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_I[0]}]
set_property -dict {PACKAGE_PIN B26 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_I[1]}]
set_property -dict {PACKAGE_PIN C25 IOSTANDARD LVCMOS33} [get_ports {MOTOR_GPIO_I[2]}]
# disc_strobe XVS/FSIN strobe out -> PMOD0 P4 (J34 pin 7, net PMOD0_IO4) -> OV9281 FSIN.
# PMOD0 aligns with MIPI1 (the other PMODs align with the other MIPI connectors). Chosen
# over the mikroBUS CS pin because J34 pin 7 (signal) sits directly above pin 9 (GND), so a
# 2-pin flylead carries signal+ground to the camera. Bank 44 VCCO = 3.3 V (VR_VCCO_43_44_
# 61_66_3V3); the signal is a direct 0R link, independent of the switched PMOD_3V3_ON rail.
set_property -dict {PACKAGE_PIN AH30 IOSTANDARD LVCMOS33} [get_ports MOTOR_XVS]
