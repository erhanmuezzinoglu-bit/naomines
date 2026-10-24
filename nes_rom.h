#ifndef NES_ROM_H
#define NES_ROM_H

#include <stdint.h>
#include "ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bank granülarite:
 *   PRG: 8KB  (MMC3 8KB banklar kullanır, esnek)
 *   CHR: 1KB  (MMC1/MMC3 1KB granülarite kullanır)
 */
#define NES_MAX_PRG_BANKS   512
#define NES_MAX_CHR_BANKS   512

int nes_rom_load_mapper0(const char *rompath, uint8_t mem_64k[0x10000], ppu_t *ppu);

/* 8KB PRG bank pointer'ları */
extern uint8_t *nes_prg_banks[NES_MAX_PRG_BANKS];
extern int nes_prg_banks_count;

/* 1KB CHR bank pointer'ları */
extern uint8_t *nes_chr_banks[NES_MAX_CHR_BANKS];
extern int nes_chr_banks_count;

extern int nes_last_mapper_loaded;
extern int nes_chr_is_rom;

/* ---------------- Mapper state ---------------- */
typedef struct {
    /* MMC1 */
    uint8_t mmc1_shift;
    uint8_t mmc1_control;
    uint8_t mmc1_chr0;
    uint8_t mmc1_chr1;
    uint8_t mmc1_prg;
    uint32_t mmc1_last_write_tick;
    uint8_t mmc1_last_write_valid;

    /* MMC3 */
    uint8_t mmc3_select;
    uint8_t mmc3_regs[8];
    uint8_t mmc3_mirroring;
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    uint8_t mmc3_irq_reload;
    uint8_t mmc3_irq_enable;
    uint8_t mmc3_prg_ram;

    /* CNROM */
    uint8_t cnrom_bank;

    /* UNROM */
    uint8_t unrom_bank;

    /* AxROM */
    uint8_t axrom_bank;

    /* MMC2 */
    uint8_t mmc2_prg_bank;
    uint8_t mmc2_chr_fd0;
    uint8_t mmc2_chr_fe0;
    uint8_t mmc2_chr_fd1;
    uint8_t mmc2_chr_fe1;
    uint8_t mmc2_latch0;
    uint8_t mmc2_latch1;
} mapper_state_t;

extern mapper_state_t g_mapper;

#ifdef __cplusplus
}
#endif

#endif