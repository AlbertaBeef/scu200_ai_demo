# Copyright 2025 mariobergeron.com
#
# Top-level Makefile: build the Vitis standalone application and gather the boot image
# into a zip under bootimages/. The SCU200 AI demo is MicroBlaze bare-metal (no PetaLinux).
#
# Flow:  make bootimage TARGET=scu200_ai_demo01
#          -> Vitis/  (make -C Vitis bootfile ...)  -> boot .bit/.elf
#          -> bootimages/<PRJ>_<TARGET>_standalone-2025-2.zip
# (The FPGA bitstream/XSA is built separately under Vivado/ — see README Stage 1.)

RM = rm -rf
ROOT_DIR = $(shell pwd)

# defaults
.DEFAULT_GOAL := bootimage
TARGET ?= scu200_ai_demo01
JOBS ?= 8

# valid targets  { template_name  both|baremetal_only }
# UPDATER START
BD_NAME = $(TARGET)
PRJ_NAME = scu200_ai_demo
COMBINE_BIT_ELF = false
scu200_target := microblaze baremetal_only
scu200_ai_demo01_target := microblaze baremetal_only
scu200_ai_demo02_target := microblaze baremetal_only
scu200_ai_demo03_target := microblaze baremetal_only
# UPDATER END

TARGET_LIST := $(sort $(patsubst %_target,%,$(filter %_target,$(.VARIABLES))))

# Vitis paths / boot output
VIT_ROOT = $(ROOT_DIR)/Vitis
VIT_BOOT_TARG = $(VIT_ROOT)/boot/$(TARGET)

# outputs
BOOTIMAGE_DIR = $(ROOT_DIR)/bootimages
BARE_ZIP = $(BOOTIMAGE_DIR)/$(PRJ_NAME)_$(TARGET)_standalone-2025-2.zip
BOOTIMAGE_LOCK = $(ROOT_DIR)/.$(TARGET).lock

# baremetal boot file: COMBINE_BIT_ELF selects the combined .bit vs the plain .bit
ifeq ($(COMBINE_BIT_ELF), true)
  VIT_BOOT_FILE = $(VIT_BOOT_TARG)/$(BD_NAME)_boot.bit
else
  VIT_BOOT_FILE = $(VIT_BOOT_TARG)/$(BD_NAME).bit
endif

.PHONY: help
help:
	@echo 'Usage:'
	@echo ''
	@echo '  make bootimage TARGET=<val> JOBS=<n>'
	@echo '    Build the Vitis standalone app and gather the boot image zip.'
	@echo ''
	@echo '  make all JOBS=<n>'
	@echo '    Same as bootimage but for all targets.'
	@echo ''
	@echo '  make clean TARGET=<val> | make clean_all'
	@echo '    Delete boot image(s).'
	@echo ''
	@echo '  TARGET (default scu200_ai_demo01), one of:'
	@$(foreach targ,$(TARGET_LIST),echo "    - $(targ)";)
	@echo '  JOBS: synthesis jobs (default 8)'

.PHONY: all
all:
	@for targ in $(TARGET_LIST); do \
		$(MAKE) --no-print-directory bootimage TARGET=$${targ} JOBS=$(JOBS); \
	done

.PHONY: bootimage
bootimage: check_target
	@if [ -f $(BOOTIMAGE_LOCK) ]; then \
		echo "$(TARGET) is locked. Skipping..."; \
	else \
		touch $(BOOTIMAGE_LOCK); \
		$(MAKE) bootimage_locked TARGET=$(TARGET) JOBS=$(JOBS); \
		rm -f $(BOOTIMAGE_LOCK); \
	fi

bootimage_locked: $(BARE_ZIP)

$(BARE_ZIP): $(VIT_BOOT_FILE)
	@echo 'Gather standalone application output products for $(TARGET)'
	mkdir -p $(BOOTIMAGE_DIR)
	cd $(VIT_BOOT_TARG) && zip -r $(BARE_ZIP) .

$(VIT_BOOT_FILE):
	$(MAKE) --no-print-directory -C $(VIT_ROOT) bootfile TARGET=$(TARGET) JOBS=$(JOBS)
	@if [ ! -e $@ ]; then echo "Error: $@ was not created for $(TARGET)."; exit 1; fi

.PHONY: clean
clean: check_target
	$(RM) $(BARE_ZIP)

.PHONY: clean_all
clean_all:
	$(RM) $(BOOTIMAGE_DIR)

check_target:
ifndef $(TARGET)_target
	$(error "Please specify a TARGET. Use 'make help' to see valid targets.")
endif
