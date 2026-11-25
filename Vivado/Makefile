# Copyright 2025 mariobergeron.com
#
# This Makefile can be used to build the Vivado projects.

CP = cp -rf
MKDIR = mkdir -p
RM = rm -rf
ROOT_DIR = $(shell pwd)
VIVADO = $(XILINX_VIVADO)/bin/vivado

# defaults
.DEFAULT_GOAL := xsa
TARGET ?= scu200_ai_demo01
JOBS ?= 8

# valid targets
# UPDATER START
BD_NAME = $(TARGET)
scu200_target := xilinx.com scu200 no_accel
# SCU200 AI demo (rotating-disc MNIST). board_name stays scu200.
scu200_ai_demo01_target := xilinx.com scu200 no_accel
# demo01 + mbdpu02 (CNN with dx/dy digit-position head).
scu200_ai_demo02_target := xilinx.com scu200 no_accel
# 112x112 mono mbdpu03 (URAM, camera crop fused in; no mbpre).
scu200_ai_demo03_target := xilinx.com scu200 no_accel
# demo03 + fpgacapZero event-timeline capture (fcapz_evtcap + fcapz_mark GPIO).
scu200_ai_demo03_fcapz_target := xilinx.com scu200 no_accel
# UPDATER END

TARGET_LIST := $(sort $(patsubst %_target,%,$(filter %_target,$(.VARIABLES))))

BOARD_URL = $(word 1,$($(TARGET)_target))
BOARD_NAME = $(word 2,$($(TARGET)_target))

# Vivado paths
VIV_SCRIPT_BD = scripts/build.tcl
VIV_SCRIPT_XSA = scripts/xsa.tcl
VIV_DIR = $(TARGET)
IP_DIR= ./ip
IP_DONE = $(IP_DIR)/build/$(BOARD_NAME)/ip_done.txt

# output files
VIV_XPR = $(VIV_DIR)/$(TARGET).xpr
VIV_XSA = $(VIV_DIR)/$(BD_NAME)_wrapper.xsa
VIV_LOG_DIR = logs
VIV_XPR_LOG = $(VIV_LOG_DIR)/$(TARGET)_xpr.log
VIV_XPR_JOU = $(VIV_LOG_DIR)/$(TARGET)_xpr.jou
VIV_XSA_LOG = $(VIV_LOG_DIR)/$(TARGET)_xsa.log
VIV_XSA_JOU = $(VIV_LOG_DIR)/$(TARGET)_xsa.jou
VIV_BIT = $(VIV_DIR)/$(TARGET).runs/impl_1/$(BD_NAME)_wrapper.bit
VIV_LOCK = $(ROOT_DIR)/.$(TARGET).lock

# These macros return values from the valid target lists defined above
define get_has_accel
$(word 3,$($(1)_target))
endef

# Targets with accelerators don't need to run implementation, only synth
ifeq ($(call get_has_accel,$(TARGET)), accel)
	SYNTH_ONLY = 1
else
	SYNTH_ONLY = 0
endif

.PHONY: help
help:
	@echo 'Usage:'
	@echo ''
	@echo '  make xsa TARGET=<val> JOBS=<n>'
	@echo '    Create the Vivado project and block design, then synthesize'
	@echo '    implement and export to XSA.'
	@echo ''
	@echo '  make project TARGET=<val>'
	@echo '    Create the Vivado project and block design.'
	@echo ''
	@echo '  make clean TARGET=<val>'
	@echo '    Clean runs for target project.'
	@echo ''
	@echo '  make xsa_all JOBS=<n>'
	@echo '    Same as make xsa but for all targets.'
	@echo ''
	@echo '  make project_all'
	@echo '    Same as make project but for all targets.'
	@echo ''
	@echo '  make clean_all'
	@echo '    Same as make clean but for all targets.'
	@echo ''
	@echo 'Parameters:'
	@echo ''
	@echo '  TARGET: Name of the target design, must be one of the following:'
	@$(foreach targ,$(TARGET_LIST),echo "    - $(targ)";)
	@echo ''
	@echo '  JOBS: Optional param to set number of synthesis jobs (default 8)'
	@echo ''
	@echo 'Example usage:'
	@echo '  make xsa TARGET=$(word 1,$(TARGET_LIST))'
	@echo ''

.PHONY: all
all: xsa_all

.PHONY: ip
ip: $(IP_DONE)

$(IP_DONE):
	make -C $(IP_DIR) ip TARGET=$(TARGET)

.PHONY: xsa
xsa:
	@if [ -f $(VIV_LOCK) ]; then \
		echo "$(TARGET) is locked. Skipping..."; \
	else \
		touch $(VIV_LOCK); \
		$(MAKE) xsa_locked TARGET=$(TARGET) JOBS=$(JOBS); \
		rm -f $(VIV_LOCK); \
	fi

xsa_locked: $(VIV_XSA) check_warnings
$(VIV_XSA): $(VIV_XPR)
	$(MKDIR) $(VIV_LOG_DIR)
	$(RM) $(VIV_XSA_LOG) $(VIV_XSA_JOU)
	$(VIVADO) -mode batch -notrace -source $(VIV_SCRIPT_XSA) -log $(VIV_XSA_LOG) -journal $(VIV_XSA_JOU) -tclargs $(TARGET) $(JOBS) $(SYNTH_ONLY)

# Ignore two critical warnings that are benign AND present in the working deliverable
# (scu200_ai_demo01): Place 30-894 = SCU200 ES1 DDR-clock delay-matching in the
# tight X0Y3 region (timing still MET), and Vivado 12-1790 = HDMI eval-license notice.
# Any OTHER critical warning still fails the build.
check_warnings:
	@grep "CRITICAL WARNING:" $(VIV_XSA_LOG) | grep -vE "Place 30-894|Vivado 12-1790" && { \
		echo "Critical warnings found. Exiting."; \
		echo "For details see: $(VIV_XSA_LOG)"; \
		exit 1; \
	} || true

.PHONY: xsa_all
xsa_all:
	for targ in $(TARGET_LIST); do \
		$(MAKE) xsa TARGET=$$targ JOBS=$(JOBS); \
	done

.PHONY: project
project: check_target
	@if [ -f $(VIV_LOCK) ]; then \
		echo "$(TARGET) is locked. Skipping..."; \
	else \
		touch $(VIV_LOCK); \
		$(MAKE) project_locked TARGET=$(TARGET) JOBS=$(JOBS); \
		rm -f $(VIV_LOCK); \
	fi

project_locked: $(VIV_XPR)
$(VIV_XPR): $(IP_DONE)
	@echo 'Create Vivado project $(TARGET)'
	$(MKDIR) $(VIV_LOG_DIR)
	$(RM) $(VIV_XPR_LOG) $(VIV_XPR_JOU)
	$(VIVADO) -mode batch -notrace -source $(VIV_SCRIPT_BD) -log $(VIV_XPR_LOG) -journal $(VIV_XPR_JOU) -tclargs $(TARGET)

.PHONY: project_all
project_all:
	for targ in $(TARGET_LIST); do \
		$(MAKE) project TARGET=$$targ JOBS=$(JOBS); \
	done

.PHONY: clean
clean: check_target
	@echo 'Cleaning project $(TARGET)'
	$(RM) $(VIV_DIR)
	$(MAKE) -C $(IP_DIR) clean TARGET=$(TARGET)

.PHONY: clean_all
clean_all:
	for targ in $(TARGET_LIST); do \
		$(MAKE) clean TARGET=$$targ; \
	done
	$(MAKE) -C $(IP_DIR) clean_all
	$(RM) .Xil vivado*.log vivado*.jou

check_target:
ifndef $(TARGET)_target
	$(error "Please specify a TARGET. Use 'make help' to see valid targets.")
endif


