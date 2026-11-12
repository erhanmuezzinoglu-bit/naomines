# Nihai ürünün adı
all: naomines.bin

# Kaynak dosyalar (.c)
SRCS += main.c
SRCS += apu.c
SRCS += nes_input.c
SRCS += nes_rom.c
SRCS += nes_bus.c
SRCS += fake6502.c
SRCS += rom_browser.c
SRCS += emu_driver.c
SRCS += ppu.c
SRCS += mapper.c

# Görseller ve Font (Ana klasörde olmalı)
SRCS += background.png
SRCS += logo.png
SRCS += nes_font.ttf

# Animasyonlu Joystick ve Yeni Buton
SRCS += joy1.png
SRCS += joy2.png
SRCS += joy3.png
SRCS += btn_main.png

# Kapaklar
SRCS += cover_mario.png
SRCS += cover_zelda.png
SRCS += cover_smb3.png
SRCS += cover_castlevania3.png
SRCS += cover_duckhunt.png
SRCS += cover_excitebike.png
SRCS += cover_gradius.png
SRCS += cover_castlevania.png
SRCS += cover_kirby.png
SRCS += cover_megaman2.png
SRCS += cover_metroid.png
SRCS += cover_prince.png
SRCS += cover_punchout.png
SRCS += cover_streemerz.png
SRCS += cover_crystalmines.png
SRCS += cover_gargoyle2.png
SRCS += cover_marble.png
SRCS += cover_crazyclimber.png
SRCS += cover_deadlytowers.png
SRCS += cover_holydiver.png
SRCS += cover_littlecisyphus.png


# Temel kurallar ve tool/parametreler
include ../../Makefile.base

# Sprite Kütüphanesi
LIBS += -lnaomisprite

# OGG/Vorbis decoding
LIBS += -lvorbisfile -lvorbis -logg

# XMP Kütüphanesi (Tracker modülleri / .xm / .mod çalmak için)
LIBS += -lxmp

# FreeType ve Bağımlılıkları
ifeq (${FEATURE_FREETYPE}, 1)
LIBS += -lfreetype -lbz2 -lz -lpng16
endif

# PNG -> C -> Object derleme kuralı
build/%.o: %.png ${IMG2C_FILE}
	@mkdir -p $(dir $@)
	${IMG2C} build/$<.c --mode RGBA4444 $<
	${CC} -c build/$<.c -o $@

# TTF -> C -> Object derleme kuralı
build/%.o: %.ttf ${IMG2C_FILE}
	@mkdir -p $(dir $@)
	${IMG2C} build/$<.c $<
	${CC} -c build/$<.c -o $@

# ROMFS image
build/romfs.bin: romfs/ ${ROMFSGEN_FILE}
	${ROMFSGEN} $@ $<

# Asıl .bin dosyasını üretir.
naomines.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE} build/romfs.bin
	${MAKEROM} $@ \
		--title "Naomi NES Emulator" \
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