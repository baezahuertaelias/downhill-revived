# M0, the memory test NRO (main.c). make; nxlink -s -a <switch-ip> build/dhd_m0.nro
ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO, normally /opt/devkitpro)
endif
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
TOOLS := $(DEVKITPRO)/tools/bin
LIBNX := $(DEVKITPRO)/libnx
HOST64 := ../../host64
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := $(ARCH) -g -O2 -Wall -frounding-math -ffp-contract=off -D__SWITCH__ -I$(HOST64) -I$(LIBNX)/include
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH) -g -L$(LIBNX)/lib

build/dhd_m0.nro: build/dhd_m0.elf build/dhd_m0.nacp
	$(TOOLS)/elf2nro $< $@ --nacp=build/dhd_m0.nacp

build/dhd_m0.nacp:
	mkdir -p build
	$(TOOLS)/nacptool --create "DHD memory test" "Downhill Revived" "M0" $@

build/dhd_m0.elf: build/main.o build/fiber_arm64.o
	$(PREFIX)gcc $(LDFLAGS) $^ -lnx -lm -o $@

build/main.o: main.c
	mkdir -p build
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

build/fiber_arm64.o: $(HOST64)/fiber_arm64.c
	mkdir -p build
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -rf build
.PHONY: clean
