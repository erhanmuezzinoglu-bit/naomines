#ifndef MAPPER_H
#define MAPPER_H

#include "nes_bus.h"

void mapper_map_prg_pages(nes_bus_t *bus);
void mapper_write(uint16_t addr, uint8_t value, nes_bus_t *bus);

/* Mapper state'i ve mapper'a ait static state'i power-on/reset durumuna getirir. */
void mapper_reset_state(void);

/* MMC3 — PPU A12 rising edge. */
void mapper_scanline_clock(void);
int  mapper_irq_pending(void);
/* IRQ'nun yukselebilecegi A12 saatinin sirasi (1 = siradaki); 0 = hicbir zaman. PPU olay mesafesi icin. */
int  mapper_scanline_clocks_until_irq(void);
int  mapper_ppu_timing_sensitive(void);
int  mapper_ppu_position_sensitive(void);
void mapper_refresh_ppu_dep(void);
extern int g_prgram_ppu_dep;

/* MMC2 initial latch/register state. */
void mapper_mmc2_setup(void);
/* MMC2: PPU desen okumasi (latch tetikleme). g_ppu_mmc2 1 iken PPU her desen ust bayt okumasinda cagirir. */
extern int g_ppu_mmc2;
/* MMC5: satir IRQ'su icin PPU satir saati (A12 yerine her render satirinda cycle 3) ve $5000-$5FFF okumalari. */
extern int g_ppu_mmc5;
int mapper_read_5xxx(uint16_t addr, uint8_t *out);
void mapper_mmc5_sprite_size_changed(void);
void mapper_mmc2_ppu_read(uint16_t addr);

/* PRG-RAM erişim izinleri. */
int mapper_prg_ram_enabled(const nes_bus_t *bus);
int mapper_prg_ram_write_enabled(const nes_bus_t *bus);

#endif