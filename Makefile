.PHONY: all check-freebsd-handoff check-freebsd-handoff-selftest check-freebsd-image check-freebsd-preflight check-shellcode-layout clean

ifndef PS5_PAYLOAD_SDK
    PS5_PAYLOAD_SDK = /opt/ps5-payload-sdk/
endif

HOST_CHECK_GOALS := check-freebsd-handoff check-freebsd-handoff-selftest check-freebsd-image check-freebsd-preflight check-shellcode-layout
NON_HOST_GOALS := $(filter-out $(HOST_CHECK_GOALS),$(MAKECMDGOALS))
ifeq ($(MAKECMDGOALS),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else ifneq ($(NON_HOST_GOALS),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

BIN := bin/ps5-freebsd-loader.elf
SRC := $(wildcard source/*.c)
OBJS := $(SRC:.c=.o)

CFLAGS  := -std=c23 -Wall -Iinclude -Ishellcode_hv -Ishellcode_kernel
LDFLAGS :=

SC_HV_H := shellcode_hv/shellcode_hv.h
SC_K_H  := shellcode_kernel/shellcode_kernel.h

all: $(SC_HV_H) $(SC_K_H) $(BIN)

check-freebsd-handoff:
ifndef FREEBSD_KERNEL
	$(error FREEBSD_KERNEL=/path/to/FreeBSD/kernel is required)
endif
	python3 tools/check_freebsd_handoff.py "$(FREEBSD_KERNEL)" $(if $(FREEBSD_KENV),--kenv "$(FREEBSD_KENV)") $(if $(FREEBSD_VRAM),--vram-file "$(FREEBSD_VRAM)")

check-freebsd-handoff-selftest:
	python3 tools/check_freebsd_handoff.py --self-test

check-freebsd-image:
ifndef FREEBSD_IMAGE
	$(error FREEBSD_IMAGE=/path/to/ps5-freebsd.img is required)
endif
	python3 tools/check_freebsd_usb_image.py "$(FREEBSD_IMAGE)"

check-shellcode-layout:
	$(MAKE) -B -C shellcode_hv shellcode_hv.h
	python3 tools/check_shellcode_layout.py

check-freebsd-preflight: check-freebsd-handoff-selftest check-shellcode-layout check-freebsd-image

$(SC_HV_H):
	$(MAKE) -C shellcode_hv

$(SC_K_H):
	$(MAKE) -C shellcode_kernel

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(BIN) $(OBJS)
	$(MAKE) -C shellcode_hv clean
	$(MAKE) -C shellcode_kernel clean
