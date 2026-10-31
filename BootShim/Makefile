
ARCH              = arm
TARGET            = arm-none-eabi
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

# UEFI_BASE must match gArmTokenSpaceGuid.PcdFdBaseAddress in
# CommonDsc.dsc.inc / the FDF (mid-DRAM, clear of the low-DRAM bootloader
# payload and of the framebuffer at 0x67000000).
UEFI_BASE        ?= 0x50000000
UEFI_SIZE        ?= 0x00100000

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ -march=armv7-a -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE)

clean:
	rm -f BootShim.elf BootShim.bin
