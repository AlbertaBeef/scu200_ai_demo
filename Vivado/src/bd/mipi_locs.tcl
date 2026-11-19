
# Copyright 2025 mariobergeron.com

# MIPI pin LOC constraints are required in the configuration of the MIPI subsystem IPs.
# The following code constructs a nested dictionary that contains the MIPI pin assignments
# for each target board, which is then used by the script that builds the block diagram.

# To use the dictionary:
#   * Get the bank number:          dict get <target> <interface> bank
#   * Get the clk_p pin:            dict get <target> <interface> clk pin
#   * Get the clk_p pin name:       dict get <target> <interface> clk pin_name
#   * Get the data<0-1>_p pin:      dict get <target> <interface> data<0-1> pin
#   * Get the data<0-1>_p pin name: dict get <target> <interface> data<0-1> pin_name

dict set mipi_loc_dict zcu104 0 bank 67
dict set mipi_loc_dict zcu104 0 clk { pin F17 pin_name IO_L13P_T2L_N0_GC_QBC_67 }
dict set mipi_loc_dict zcu104 0 data0 { pin H19 pin_name IO_L15P_T2L_N4_AD11P_67 }
dict set mipi_loc_dict zcu104 0 data1 { pin L20 pin_name IO_L19P_T3L_N0_DBC_AD9P_67 }
dict set mipi_loc_dict zcu104 1 bank 67
dict set mipi_loc_dict zcu104 1 clk { pin H18 pin_name IO_L16P_T2U_N6_QBC_AD3P_67 }
dict set mipi_loc_dict zcu104 1 data0 { pin D16 pin_name IO_L7P_T1L_N0_QBC_AD13P_67 }
dict set mipi_loc_dict zcu104 1 data1 { pin C13 pin_name IO_L6P_T0U_N10_AD6P_67 }
dict set mipi_loc_dict zcu104 2 bank 68
dict set mipi_loc_dict zcu104 2 clk { pin D11 pin_name IO_L16P_T2U_N6_QBC_AD3P_68 }
dict set mipi_loc_dict zcu104 2 data0 { pin B6 pin_name IO_L21P_T3L_N4_AD8P_68 }
dict set mipi_loc_dict zcu104 2 data1 { pin F11 pin_name IO_L14P_T2L_N2_GC_68 }
dict set mipi_loc_dict zcu104 3 bank 68
dict set mipi_loc_dict zcu104 3 clk { pin F7 pin_name IO_L7P_T1L_N0_QBC_AD13P_68 }
dict set mipi_loc_dict zcu104 3 data0 { pin C9 pin_name IO_L8P_T1L_N2_AD5P_68 }
dict set mipi_loc_dict zcu104 3 data1 { pin M13 pin_name IO_L1P_T0L_N0_DBC_68 }
dict set mipi_loc_dict zcu102_hpc0 0 bank 66
dict set mipi_loc_dict zcu102_hpc0 0 clk { pin Y4 pin_name IO_L13P_T2L_N0_GC_QBC_66 }
dict set mipi_loc_dict zcu102_hpc0 0 data0 { pin AC2 pin_name IO_L19P_T3L_N0_DBC_AD9P_66 }
dict set mipi_loc_dict zcu102_hpc0 0 data1 { pin V2 pin_name IO_L23P_T3U_N8_66 }
dict set mipi_loc_dict zcu102_hpc0 1 bank 66
dict set mipi_loc_dict zcu102_hpc0 1 clk { pin AB4 pin_name IO_L16P_T2U_N6_QBC_AD3P_66 }
dict set mipi_loc_dict zcu102_hpc0 1 data0 { pin Y10 pin_name IO_L6P_T0U_N10_AD6P_66 }
dict set mipi_loc_dict zcu102_hpc0 1 data1 { pin AC7 pin_name IO_L7P_T1L_N0_QBC_AD13P_66 }
dict set mipi_loc_dict zcu102_hpc0 2 bank 67
dict set mipi_loc_dict zcu102_hpc0 2 clk { pin N9 pin_name IO_L16P_T2U_N6_QBC_AD3P_67 }
dict set mipi_loc_dict zcu102_hpc0 2 data0 { pin L12 pin_name IO_L18P_T2U_N10_AD2P_67 }
dict set mipi_loc_dict zcu102_hpc0 2 data1 { pin P11 pin_name IO_L13P_T2L_N0_GC_QBC_67 }
dict set mipi_loc_dict zcu102_hpc0 3 bank 67
dict set mipi_loc_dict zcu102_hpc0 3 clk { pin V8 pin_name IO_L7P_T1L_N0_QBC_AD13P_67 }
dict set mipi_loc_dict zcu102_hpc0 3 data0 { pin V12 pin_name IO_L5P_T0U_N8_AD14P_67 }
dict set mipi_loc_dict zcu102_hpc0 3 data1 { pin T7 pin_name IO_L10P_T1U_N6_QBC_AD4P_67 }
dict set mipi_loc_dict zcu102_hpc1 0 bank 65
dict set mipi_loc_dict zcu102_hpc1 0 clk { pin AE5 pin_name IO_L13P_T2L_N0_GC_QBC_65 }
dict set mipi_loc_dict zcu102_hpc1 0 data0 { pin AH2 pin_name IO_L19P_T3L_N0_DBC_AD9P_65 }
dict set mipi_loc_dict zcu102_hpc1 0 data1 { pin AD2 pin_name IO_L23P_T3U_N8_I2C_SCLK_65 }
dict set mipi_loc_dict zcu102_hpc1 1 bank 65
dict set mipi_loc_dict zcu102_hpc1 1 clk { pin AJ6 pin_name IO_L16P_T2U_N6_QBC_AD3P_65 }
dict set mipi_loc_dict zcu102_hpc1 1 data0 { pin AD10 pin_name IO_L6P_T0U_N10_AD6P_65 }
dict set mipi_loc_dict zcu102_hpc1 1 data1 { pin AH7 pin_name IO_L7P_T1L_N0_QBC_AD13P_65 }
dict set mipi_loc_dict zcu106_hpc0 0 bank 67
dict set mipi_loc_dict zcu106_hpc0 0 clk { pin F17 pin_name IO_L13P_T2L_N0_GC_QBC_67 }
dict set mipi_loc_dict zcu106_hpc0 0 data0 { pin H19 pin_name IO_L15P_T2L_N4_AD11P_67 }
dict set mipi_loc_dict zcu106_hpc0 0 data1 { pin L20 pin_name IO_L19P_T3L_N0_DBC_AD9P_67 }
dict set mipi_loc_dict zcu106_hpc0 1 bank 67
dict set mipi_loc_dict zcu106_hpc0 1 clk { pin H18 pin_name IO_L16P_T2U_N6_QBC_AD3P_67 }
dict set mipi_loc_dict zcu106_hpc0 1 data0 { pin D16 pin_name IO_L7P_T1L_N0_QBC_AD13P_67 }
dict set mipi_loc_dict zcu106_hpc0 1 data1 { pin C13 pin_name IO_L6P_T0U_N10_AD6P_67 }
dict set mipi_loc_dict zcu106_hpc0 2 bank 68
dict set mipi_loc_dict zcu106_hpc0 2 clk { pin D11 pin_name IO_L16P_T2U_N6_QBC_AD3P_68 }
dict set mipi_loc_dict zcu106_hpc0 2 data0 { pin B6 pin_name IO_L21P_T3L_N4_AD8P_68 }
dict set mipi_loc_dict zcu106_hpc0 2 data1 { pin F11 pin_name IO_L14P_T2L_N2_GC_68 }
dict set mipi_loc_dict zcu106_hpc0 3 bank 68
dict set mipi_loc_dict zcu106_hpc0 3 clk { pin F7 pin_name IO_L7P_T1L_N0_QBC_AD13P_68 }
dict set mipi_loc_dict zcu106_hpc0 3 data0 { pin C9 pin_name IO_L8P_T1L_N2_AD5P_68 }
dict set mipi_loc_dict zcu106_hpc0 3 data1 { pin M13 pin_name IO_L1P_T0L_N0_DBC_68 }
dict set mipi_loc_dict uzev 0 bank 64
dict set mipi_loc_dict uzev 0 clk { pin AF16 pin_name IO_L13P_T2L_N0_GC_QBC_64 }
dict set mipi_loc_dict uzev 0 data0 { pin AC17 pin_name IO_L17P_T2U_N8_AD10P_64 }
dict set mipi_loc_dict uzev 0 data1 { pin AG18 pin_name IO_L23P_T3U_N8_64 }
dict set mipi_loc_dict uzev 1 bank 64
dict set mipi_loc_dict uzev 1 clk { pin AG13 pin_name IO_L7P_T1L_N0_QBC_AD13P_64 }
dict set mipi_loc_dict uzev 1 data0 { pin AK13 pin_name IO_L8P_T1L_N2_AD5P_64 }
dict set mipi_loc_dict uzev 1 data1 { pin AF15 pin_name IO_L12P_T1U_N10_GC_64 }
dict set mipi_loc_dict uzev 2 bank 65
dict set mipi_loc_dict uzev 2 clk { pin AH6 pin_name IO_L13P_T2L_N0_GC_QBC_65 }
dict set mipi_loc_dict uzev 2 data0 { pin AK7 pin_name IO_L15P_T2L_N4_AD11P_65 }
dict set mipi_loc_dict uzev 2 data1 { pin AG6 pin_name IO_L14P_T2L_N2_GC_65 }
dict set mipi_loc_dict uzev 3 bank 65
dict set mipi_loc_dict uzev 3 clk { pin AK9 pin_name IO_L7P_T1L_N0_QBC_AD13P_65 }
dict set mipi_loc_dict uzev 3 data0 { pin AG11 pin_name IO_L3P_T0L_N4_AD15P_65 }
dict set mipi_loc_dict uzev 3 data1 { pin AJ11 pin_name IO_L4P_T0U_N6_DBC_AD7P_SMBALERT_65 }
dict set mipi_loc_dict pynqzu 0 bank 65
dict set mipi_loc_dict pynqzu 0 clk { pin L7 pin_name IO_L13P_T2L_N0_GC_QBC_65 }
dict set mipi_loc_dict pynqzu 0 data0 { pin J7 pin_name IO_L21P_T3L_N4_AD8P_65 }
dict set mipi_loc_dict pynqzu 0 data1 { pin K8 pin_name IO_L22P_T3U_N6_DBC_AD0P_65 }
dict set mipi_loc_dict pynqzu 1 bank 65
dict set mipi_loc_dict pynqzu 1 clk { pin P7 pin_name IO_L16P_T2U_N6_QBC_AD3P_65 }
dict set mipi_loc_dict pynqzu 1 data0 { pin N9 pin_name IO_L17P_T2U_N8_AD10P_65 }
dict set mipi_loc_dict pynqzu 1 data1 { pin R7 pin_name IO_L5P_T0U_N8_AD14P_65 }
dict set mipi_loc_dict pynqzu 2 bank 64
dict set mipi_loc_dict pynqzu 2 clk { pin AD2 pin_name IO_L16P_T2U_N6_QBC_AD3P_64 }
dict set mipi_loc_dict pynqzu 2 data0 { pin AG3 pin_name IO_L20P_T3L_N2_AD1P_64 }
dict set mipi_loc_dict pynqzu 2 data1 { pin AC4 pin_name IO_L14P_T2L_N2_GC_64 }
dict set mipi_loc_dict pynqzu 3 bank 64
dict set mipi_loc_dict pynqzu 3 clk { pin AG9 pin_name IO_L7P_T1L_N0_QBC_AD13P_64 }
dict set mipi_loc_dict pynqzu 3 data0 { pin AD7 pin_name IO_L4P_T0U_N6_DBC_AD7P_64 }
dict set mipi_loc_dict pynqzu 3 data1 { pin AG6 pin_name IO_L10P_T1U_N6_QBC_AD4P_64 }
dict set mipi_loc_dict genesyszu 0 bank 65
dict set mipi_loc_dict genesyszu 0 clk { pin L7 pin_name IO_L13P_T2L_N0_GC_QBC_65 }
dict set mipi_loc_dict genesyszu 0 data0 { pin L1 pin_name IO_L7P_T1L_N0_QBC_AD13P_65 }
dict set mipi_loc_dict genesyszu 0 data1 { pin J6 pin_name IO_L20P_T3L_N2_AD1P_65 }
dict set mipi_loc_dict genesyszu 1 bank 65
dict set mipi_loc_dict genesyszu 1 clk { pin H4 pin_name IO_L10P_T1U_N6_QBC_AD4P_65 }
dict set mipi_loc_dict genesyszu 1 data0 { pin R8 pin_name IO_L4P_T0U_N6_DBC_AD7P_SMBALERT_65 }
dict set mipi_loc_dict genesyszu 1 data1 { pin P7 pin_name IO_L16P_T2U_N6_QBC_AD3P_65 }
dict set mipi_loc_dict genesyszu 2 bank 64
dict set mipi_loc_dict genesyszu 2 clk { pin AG9 pin_name IO_L7P_T1L_N0_QBC_AD13P_64 }
dict set mipi_loc_dict genesyszu 2 data0 { pin AF7 pin_name IO_L11P_T1U_N8_GC_64 }
dict set mipi_loc_dict genesyszu 2 data1 { pin AD2 pin_name IO_L16P_T2U_N6_QBC_AD3P_64 }
dict set mipi_loc_dict genesyszu 3 bank 64
dict set mipi_loc_dict genesyszu 3 clk { pin AC9 pin_name IO_L1P_T0L_N0_DBC_64 }
dict set mipi_loc_dict genesyszu 3 data0 { pin AB6 pin_name IO_L6P_T0U_N10_AD6P_64 }
dict set mipi_loc_dict genesyszu 3 data1 { pin AE9 pin_name IO_L2P_T0L_N2_64 }
dict set mipi_loc_dict auboard 0 bank 66
dict set mipi_loc_dict auboard 0 clk { pin F24 pin_name IO_L16P_T2U_N6_QBC_AD3P_66 }
dict set mipi_loc_dict auboard 0 data0 { pin D26 pin_name IO_L23P_T3U_N8_66 }
dict set mipi_loc_dict auboard 0 data1 { pin H26 pin_name IO_L17P_T2U_N8_AD10P_66 }
dict set mipi_loc_dict auboard 1 bank 66
dict set mipi_loc_dict auboard 1 clk { pin L24 pin_name IO_L10P_T1U_N6_QBC_AD4P_66 }
dict set mipi_loc_dict auboard 1 data0 { pin K21 pin_name IO_L5P_T0U_N8_AD14P_66 }
dict set mipi_loc_dict auboard 1 data1 { pin L20 pin_name IO_L6P_T0U_N10_AD6P_66 }
dict set mipi_loc_dict auboard 2 bank 66
dict set mipi_loc_dict auboard 2 clk { pin G24 pin_name IO_L13P_T2L_N0_GC_QBC_66 }
dict set mipi_loc_dict auboard 2 data0 { pin H23 pin_name IO_L14P_T2L_N2_GC_66 }
dict set mipi_loc_dict auboard 2 data1 { pin J25 pin_name IO_L15P_T2L_N4_AD11P_66 }
dict set mipi_loc_dict auboard 3 bank 66
dict set mipi_loc_dict auboard 3 clk { pin L22 pin_name IO_L7P_T1L_N0_QBC_AD13P_66 }
dict set mipi_loc_dict auboard 3 data0 { pin M25 pin_name IO_L8P_T1L_N2_AD5P_66 }
dict set mipi_loc_dict auboard 3 data1 { pin K25 pin_name IO_L9P_T1L_N4_AD12P_66 }

# SCU200: on-board RPi/MIPI camera connectors (HPIO bank 45).
# Camera 0 -> connector MIPI1, camera 2 -> connector MIPI2 (2-lane each).
dict set mipi_loc_dict scu200 0 bank 45
dict set mipi_loc_dict scu200 0 clk { pin Y27 pin_name IO_L1P_T0L_N0_DBC_45 }
dict set mipi_loc_dict scu200 0 data0 { pin U26 pin_name IO_L2P_T0L_N2_45 }
dict set mipi_loc_dict scu200 0 data1 { pin Y29 pin_name IO_L3P_T0L_N4_45 }
# MIPI1 (J31) routes all 4 data lanes; D2/D3 for the 4-lane IMX477 config.
dict set mipi_loc_dict scu200 0 data2 { pin W26 pin_name IO_L4P_T0U_N6_DBC_45 }
dict set mipi_loc_dict scu200 0 data3 { pin W30 pin_name IO_L5P_T0U_N8_45 }
dict set mipi_loc_dict scu200 2 bank 45
dict set mipi_loc_dict scu200 2 clk { pin T29 pin_name IO_L13P_T2L_N0_GC_QBC_45 }
dict set mipi_loc_dict scu200 2 data0 { pin T31 pin_name IO_L14P_T2L_N2_GC_45 }
dict set mipi_loc_dict scu200 2 data1 { pin R27 pin_name IO_L15P_T2L_N4_45 }
# MIPI2 (J26) 4-lane: D2/D3 for the swapped IMX477 4-lane test.
dict set mipi_loc_dict scu200 2 data2 { pin R32 pin_name IO_L16P_T2U_N6_QBC_45 }
dict set mipi_loc_dict scu200 2 data3 { pin R30 pin_name IO_L17P_T2U_N8_45 }

# scu200_ai_demo01: rotating-disc MNIST demo — single camera on MIPI1 (index 0),
# same D-PHY pinout as the base scu200 board.
dict set mipi_loc_dict scu200_ai_demo01 0 [dict get $mipi_loc_dict scu200 0]
# scu200_ai_demo02: demo01 + mbdpu02 (CNN with digit-position head) — same camera/pinout.
dict set mipi_loc_dict scu200_ai_demo02 0 [dict get $mipi_loc_dict scu200 0]
