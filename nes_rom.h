#ifndef NES_ROM_H
#define NES_ROM_H

#include <stdint.h>
#include "ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iNES Mapper 0 (NROM), Mapper 2 (UNROM) veya Mapper 3 (CNROM) yükler:
   - PRG -> emu bank yapısına alınır.
   - CHR -> PPU VRAM veya Bank yapısına alınır.
*/
int nes_rom_load_mapper0(const char *rompath, uint8_t mem_64k[0x10000], ppu_t *ppu);

/* --- UNROM (mapper2) emülasyonu için diziler ve state --- */
#define NES_UNROM_MAX_BANKS 16
extern uint8_t *nes_unrom_prg_banks[NES_UNROM_MAX_BANKS];
extern int nes_unrom_prg_banks_count;
extern int nes_unrom_current_bank;

/* --- CNROM (mapper3) emülasyonu için diziler ve state --- */
extern uint8_t *nes_cnrom_chr_banks[128]; 
extern int nes_cnrom_chr_banks_count;

/* Son yüklenen mapper tipi (0, 2 veya 3) */
extern int nes_last_mapper_loaded;

/* CHR write koruma flag'i (AccuracyCoin 16-1 fix):
   1 = CHR ROM (yazılamaz) — chr_banks > 0 olan ROM'lar
   0 = CHR RAM (yazılabilir) — chr_banks == 0 olan ROM'lar
*/
extern int nes_chr_is_rom;

#ifdef __cplusplus
}
#endif

#endif