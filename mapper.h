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

/* MMC2 initial latch/register state. */
void mapper_mmc2_setup(void);

/* PRG-RAM erişim izinleri. */
int mapper_prg_ram_enabled(const nes_bus_t *bus);
int mapper_prg_ram_write_enabled(const nes_bus_t *bus);

#endif