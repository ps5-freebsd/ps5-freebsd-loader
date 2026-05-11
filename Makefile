.PHONY: all clean

ifndef PS5_PAYLOAD_SDK
    PS5_PAYLOAD_SDK = /opt/ps5-payload-sdk/
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

BIN := bin/ps5-linux-loader.elf
SRC := $(wildcard source/*.c)
OBJS := $(SRC:.c=.o)

CFLAGS  := -std=c23 -Wall -Iinclude -Ishellcode_hypervisor -Ishellcode_kernel
LDFLAGS :=

SC_HV_H := shellcode_hypervisor/shellcode_hypervisor.h
SC_K_H  := shellcode_kernel/shellcode_kernel.h

all: $(SC_HV_H) $(SC_K_H) $(BIN)

$(SC_HV_H):
	$(MAKE) -C shellcode_hypervisor

$(SC_K_H):
	$(MAKE) -C shellcode_kernel

source/firmware.o: scripts/ubuntu/initramfs-tools scripts/ubuntu/ps5-wifi-fw

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(BIN) $(OBJS)
	$(MAKE) -C shellcode_hypervisor clean
	$(MAKE) -C shellcode_kernel clean
