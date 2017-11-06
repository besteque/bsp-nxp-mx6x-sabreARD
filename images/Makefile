ROOT_DIR := $(notdir $(CURDIR))
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

HOST_MKIFS := mkifs

SUFFIXES := .build .ifs .raw

.PHONY: all clean

all: ipl-mx6x-sabreARD.bin ifs-mx6x-sabreARD.bin ifs-mx6x-sabreARD-graphics.bin

clean: 
	$(RM_HOST) ifs-mx6x-sabreARD.bin ipl-mx6x-sabreARD.bin
	$(RM_HOST) ifs-mx6x-sabreARD-graphics.bin

ifs-mx6x-sabreARD.bin: sabreARD.build
	$(HOST_MKIFS) -vvv -r../install $(MKIFSFLAGS) $^ $@

ifs-mx6x-sabreARD-graphics.bin: sabreARD-graphics.build
	$(HOST_MKIFS) -r../install -v $(MKIFSFLAGS) $^ $@

ipl-mx6x-sabreARD.bin:
	$(SHELL) ./mkflashimage.sh
