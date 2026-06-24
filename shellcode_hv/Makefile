ifeq ($(shell uname -m),aarch64)
CC      = x86_64-linux-gnu-gcc
LD      = x86_64-linux-gnu-ld
OBJCOPY = x86_64-linux-gnu-objcopy
else
CC      = gcc
LD      = ld
OBJCOPY = objcopy
endif
CFLAGS  = -O2 -fno-stack-protector -ffreestanding -nostdlib -fcf-protection=none -mno-sse -mno-sse2 -m64
LDFLAGS = -T linker.ld -Wl,--no-warn-rwx-segments
TARGET  = shellcode_hv.elf
TEXT_BIN = shellcode_hv.bin
dump = shellcode_hv.h
BIN2C_NAME = $(subst .,_,$(TEXT_BIN))

SRC = main.c utils.c boot_freebsd.c
OBJ = $(SRC:.c=.o)

all: $(dump)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TEXT_BIN): $(TARGET)
	$(OBJCOPY) -O binary -j .shell_code $(TARGET) $(TEXT_BIN)

clean:
	rm -f $(OBJ) $(TARGET) $(TEXT_BIN) $(dump)

$(dump): $(TEXT_BIN)
	@if command -v xxd >/dev/null 2>&1; then \
		xxd -i $(TEXT_BIN) > $(dump); \
	else \
		od -An -tx1 -v $(TEXT_BIN) | awk 'BEGIN { n = 0; printf "unsigned char $(BIN2C_NAME)[] = {\n" } { for (i = 1; i <= NF; i++) { printf "  0x%s,", $$i; n++; if (n % 12 == 0) printf "\n" } } END { printf "\n};\nunsigned int $(BIN2C_NAME)_len = %d;\n", n }' > $(dump); \
	fi
