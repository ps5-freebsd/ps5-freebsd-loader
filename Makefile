.PHONY: all clean

ifndef PS5_PAYLOAD_SDK
    PS5_PAYLOAD_SDK = /opt/ps5-payload-sdk/
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

BIN := bin/ps5-freebsd-loader.elf
SRC := $(wildcard source/*.c)
OBJS := $(SRC:.c=.o)

CFLAGS  := -std=c23 -Wall -Iinclude -Ishellcode_hv -Ishellcode_kernel
LDFLAGS :=

SC_HV_H := shellcode_hv/shellcode_hv.h
SC_K_H  := shellcode_kernel/shellcode_kernel.h

all: $(SC_HV_H) $(SC_K_H) $(BIN)

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
