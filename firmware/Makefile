# STK600-clone replacement firmware (STC8H8K64U) + host pre-flash-gate tools.
#
#   make host    - build pty harness + unit tests (gcc)
#   make test    - run unit tests
#   make gate    - run the pre-flash gate (real avrdude vs simulated target)
#   make fw      - build target firmware (SDCC) -> build/fw.bin
#   make flash   - flash via stc8usb (guarded; erases stock firmware FOREVER)

SDCC    ?= sdcc
MAKEBIN ?= makebin
STC8USB ?= stc8usb
CC      ?= gcc

BUILD := build

CORE_SRC := src/stk500v2.c src/hvpp.c src/isp.c
FW_SRC   := $(CORE_SRC) src/uart2.c src/usb_cdc.c src/hal_stc8.c src/adc.c src/pindbg.c src/main.c
FW_RELS  := $(patsubst src/%.c,$(BUILD)/rel/%.rel,$(FW_SRC))

SDCCFLAGS  := -mmcs51 --model-small --std-c99 --opt-code-size \
              --xram-size 8192 --code-size 65536 -Isrc
HOSTCFLAGS := -O1 -g -Wall -Wextra -Isrc

.PHONY: all host fw test gate flash clean

all: host fw

# ---- host (gcc) ----

host: $(BUILD)/pty_harness $(BUILD)/test_remap

$(BUILD)/pty_harness: host/pty_main.c host/hal_mock.c $(CORE_SRC) src/*.h | $(BUILD)
	$(CC) $(HOSTCFLAGS) host/pty_main.c host/hal_mock.c $(CORE_SRC) -o $@

$(BUILD)/test_remap: host/test_remap.c host/hal_mock.c $(CORE_SRC) src/*.h | $(BUILD)
	$(CC) $(HOSTCFLAGS) host/test_remap.c host/hal_mock.c $(CORE_SRC) -o $@

test: $(BUILD)/test_remap
	$(BUILD)/test_remap

gate: host
	test/run_gate.sh

# ---- target (SDCC) ----

fw: $(BUILD)/fw.bin
	@echo "code size:" && grep -E "^(CODE|XRAM|ROM)" $(BUILD)/fw.mem || true

$(BUILD)/rel/%.rel: src/%.c src/*.h | $(BUILD)
	$(SDCC) $(SDCCFLAGS) -c $< -o $@

$(BUILD)/fw.ihx: $(FW_RELS)
	$(SDCC) $(SDCCFLAGS) $(FW_RELS) -o $@

$(BUILD)/fw.bin: $(BUILD)/fw.ihx
	$(MAKEBIN) -p $< $@

flash: $(BUILD)/fw.bin
	@echo "!!! First flash ERASES the stock firmware PERMANENTLY."
	@echo "!!! Prerequisites: 'make gate' green + explicit go decision."
	@test "$(CONFIRM)" = yes || { echo "Refusing. Run: make flash CONFIRM=yes"; exit 1; }
	$(STC8USB) -t 24000000 -f $(BUILD)/fw.bin

$(BUILD):
	mkdir -p $(BUILD)/rel

clean:
	rm -rf $(BUILD)
