# Nihai ürünün adı
all: naomines.bin

# Kaynak dosyalar.
SRCS += main.c
SRCS += apu.c
SRCS += trace.c
SRCS += nes_input.c
SRCS += nes_rom.c
SRCS += nes_bus.c
SRCS += debug_ui.c
SRCS += fake6502.c
SRCS += rom_browser.c
SRCS += emu_driver.c
SRCS += debug_hud.c
SRCS += ppu.c
SRCS += opc.c
SRCS += mapper.c

# Temel kurallar ve tool/parametreler
include ../../Makefile.base

# ROMFS image (romfs/ altını otomatik image yapar)
build/romfs.bin: romfs/ ${ROMFSGEN_FILE}
	${ROMFSGEN} $@ $<

# Asıl .bin dosyasını üretir.
naomines.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE} build/romfs.bin
	${MAKEROM} $@ \
		--title "Naomi NES Explorer" \
		--publisher "erhanmuezzinoglu" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR} \
		--align-before-data 4 \
		--filedata build/romfs.bin

.PHONY: clean
clean:
	rm -rf build
	rm -f naomines.bin