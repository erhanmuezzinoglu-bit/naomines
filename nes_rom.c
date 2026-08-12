#include "nes_rom.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Global Değişkenler --- */
uint8_t *nes_unrom_prg_banks[NES_UNROM_MAX_BANKS] = {0};
int nes_unrom_prg_banks_count = 0;
int nes_unrom_current_bank = 0;

uint8_t *nes_cnrom_chr_banks[128] = {0};
int nes_cnrom_chr_banks_count = 0;

int nes_last_mapper_loaded = 0;

/* CHR write koruma flag'i — AccuracyCoin 16-1 fix */
int nes_chr_is_rom = 0;

static uint8_t *last_unrom_heap = NULL;
static uint8_t *last_chr_heap = NULL;

static int ines_valid(const uint8_t hdr[16]) {
    return (hdr[0] == 'N' && hdr[1] == 'E' && hdr[2] == 'S' && hdr[3] == 0x1A);
}

static uint8_t ines_mapper(const uint8_t hdr[16]) {
    uint8_t mapper = (hdr[7] & 0xF0) | ((hdr[6] & 0xF0) >> 4);
    if ((hdr[7] & 0x0C) == 0x08) {
        mapper |= (hdr[8] & 0x0F) << 8;
    }
    return mapper;
}

int nes_rom_load_mapper0(const char *rompath, uint8_t mem_64k[0x10000], ppu_t *ppu) {
    FILE *fp = fopen(rompath, "rb");
    if (!fp) return 0;

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, fp) != 16 || !ines_valid(hdr)) {
        fclose(fp);
        return 0;
    }

    uint8_t prg_banks = hdr[4];
    uint8_t chr_banks = hdr[5];
    uint8_t flags6 = hdr[6];
    uint16_t mapper = ines_mapper(hdr);
    nes_last_mapper_loaded = mapper;

    if (ppu) {
        ppu->mirroring = (flags6 & 0x01) ? 1 : 0; 
    }

    long prg_off = 16 + ((flags6 & 0x04) ? 512 : 0);
    fseek(fp, prg_off, SEEK_SET);

    if (last_unrom_heap) { free(last_unrom_heap); last_unrom_heap = NULL; }
    if (last_chr_heap) { free(last_chr_heap); last_chr_heap = NULL; }
    for(int i=0; i<NES_UNROM_MAX_BANKS; i++) nes_unrom_prg_banks[i] = NULL;
    for(int i=0; i<128; i++) nes_cnrom_chr_banks[i] = NULL;
    nes_cnrom_chr_banks_count = 0;
    nes_unrom_prg_banks_count = prg_banks;

    uint32_t prg_len = (uint32_t)prg_banks * 16384;
    last_unrom_heap = (uint8_t*)malloc(prg_len);
    fread(last_unrom_heap, 1, prg_len, fp);

    for (int i = 0; i < prg_banks && i < NES_UNROM_MAX_BANKS; i++) {
        nes_unrom_prg_banks[i] = last_unrom_heap + (i * 16384);
    }

    if (prg_banks == 1) {
        memcpy(&mem_64k[0x8000], nes_unrom_prg_banks[0], 16384);
        memcpy(&mem_64k[0xC000], nes_unrom_prg_banks[0], 16384);
    } else {
        memcpy(&mem_64k[0x8000], nes_unrom_prg_banks[0], 16384);
        memcpy(&mem_64k[0xC000], nes_unrom_prg_banks[prg_banks - 1], 16384);
    }

    /* --- CHR Yükleme + ROM/RAM tespiti --- */
    if (ppu) {
        if (chr_banks > 0) {
            // CHR ROM mevcut — yazılamaz
            nes_chr_is_rom = 1;

            uint32_t chr_total_size = (uint32_t)chr_banks * 8192;
            last_chr_heap = (uint8_t*)malloc(chr_total_size);
            
            fseek(fp, prg_off + prg_len, SEEK_SET);
            fread(last_chr_heap, 1, chr_total_size, fp);

            if (mapper == 3) {
                nes_cnrom_chr_banks_count = chr_banks;
                for (int i = 0; i < chr_banks && i < 128; i++) {
                    nes_cnrom_chr_banks[i] = last_chr_heap + (i * 8192);
                }
                memcpy(ppu->vram, nes_cnrom_chr_banks[0], 8192);
            } else {
                memcpy(ppu->vram, last_chr_heap, 8192);
            }
        } else {
            // CHR RAM — yazılabilir
            nes_chr_is_rom = 0;
            memset(ppu->vram, 0, 8192);
        }
        ppu->dirty_all = 1;
    }

    fclose(fp);
    printf("ROM Yuklendi: Mapper %u, PRG Banks: %d, CHR Banks: %d, CHR_IS_ROM=%d\n",
           mapper, prg_banks, chr_banks, nes_chr_is_rom);
    return 1;
}