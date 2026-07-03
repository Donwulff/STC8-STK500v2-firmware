# ATtiny461A on-target self-test — build + flash through the STC8 programmer.
#
#   make            build build/selftest.hex (full suite)
#   make size       section sizes
#   make flash      program via avrdude over the board's ISP path
#   make run        flash + drive + decode results (host/run_selftest.py)
#
# Feature trimming (see src/config.h):
#   make FEATURES='-DCFG_TEST_EEPROM=0 -DCFG_TEST_WDT=0'

MCU          = attiny461a
F_CPU        = 1000000UL        # factory fuses: 8 MHz RC / CKDIV8
FLASH_SIZE   = 4096
AVRDUDE_PART = t461a
PORT        ?= /dev/ttyACM0
FEATURES    ?=

CC      = avr-gcc
OBJCOPY = avr-objcopy

SRC  = src/main.c src/flash_crc.c src/eeprom_test.c src/adc_check.c
ASRC = src/march.S src/cpu_regs.S

CFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -Wall -Wextra -std=gnu11 -Isrc \
         $(FEATURES)

all: build/selftest.hex size

build:
	mkdir -p build

build/selftest.elf: $(SRC) $(ASRC) src/config.h | build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ASRC)

# elf -> raw bin -> pad + CRC16 reference in last 2 bytes -> ihex
build/selftest_nocrc.bin: build/selftest.elf
	$(OBJCOPY) -O binary --gap-fill 0xFF $< $@

build/selftest.bin: build/selftest_nocrc.bin tools/patch_crc.py
	python3 tools/patch_crc.py $< $@ $(FLASH_SIZE)

build/selftest.hex: build/selftest.bin
	$(OBJCOPY) -I binary -O ihex $< $@

size: build/selftest.elf
	avr-size $<

flash: build/selftest.hex
	avrdude -c stk500v2 -P $(PORT) -p $(AVRDUDE_PART) -U flash:w:build/selftest.hex:i

run: build/selftest.hex
	python3 host/run_selftest.py --port $(PORT) --hex build/selftest.hex

clean:
	rm -rf build

.PHONY: all size flash run clean
