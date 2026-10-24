#ifndef NES_BUS_H
#define NES_BUS_H

#include <stdint.h>
#include "fake6502.h"
#include "ppu.h"
#include "nes_input.h"
#include "apu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes_bus
{
    /* Bellek Alanları */
    uint8_t mem[0x10000];
    uint8_t cart_sram[0x2000];

    /* Bileşen Bağlantıları */
    ppu_t *ppu;
    nes_input_t *in;

    /* --- OPEN BUS & DYNAMIC DATA --- */
    uint8_t open_bus_value;

    /* CPU erisimlerinin gordugu ic veri yolu. */
    uint8_t cpu_internal_bus_value;

    /* DMC DMA halt/alignment dummy read için. */
    uint16_t last_cpu_read_addr;

    /* --- APU Durumu ve Senkronizasyon --- */
    apu_t apu;

    uint32_t last_apu_sync_tick;
    uint32_t last_ppu_sync_tick;

    /* --- OAM DMA REQUEST STATE --- */
    uint8_t oam_dma_pending;
    uint8_t oam_dma_page;

} nes_bus_t;


void nes_bus_init(
    nes_bus_t *b,
    ppu_t *ppu,
    nes_input_t *in
);

uint8_t nes_bus_read(
    nes_bus_t *b,
    fake6502_context *ctx,
    uint16_t addr
);

void nes_bus_write(
    nes_bus_t *b,
    fake6502_context *ctx,
    uint16_t addr,
    uint8_t value
);

/* ---- RAM / PRG KISA DEVRELERI (nes_bus_read / nes_bus_write ile AYNI yan etkiler) ----
 * Okuma: $0000-$1FFF (RAM) ve $8000-$FFFF (PRG). nes_bus_read bu adreslerde yalnizca
 *   last_cpu_read_addr, pad1_last_bus_was_4016=0, open_bus_value ve cpu_internal_bus_value
 *   atar ve bus->mem'den okur (APU/PPU senkronu ve mapper cagrisi yok).
 * Yazma: $0000-$1FFF. nes_bus_write yalnizca pad1_last_bus_was_4016=0, open_bus_value,
 *   cpu_internal_bus_value ve bus->mem yazimi yapar.
 * Donus: 1 = kisa devre uygulandi, 0 = uygulanmadi (normal nes_bus_read/write cagrilmali). */
static inline int nes_bus_fast_read(nes_bus_t *b, uint16_t addr, uint8_t *out)
{
    uint8_t v;

    if (addr < 0x2000)       v = b->mem[addr & 0x07FF];
    else if (addr >= 0x8000) v = b->mem[addr];
    else                     return 0;

    b->last_cpu_read_addr = addr;
    if (b->in) b->in->pad1_last_bus_was_4016 = 0;
    b->open_bus_value = v;
    b->cpu_internal_bus_value = v;
    *out = v;
    return 1;
}

static inline int nes_bus_fast_write(nes_bus_t *b, uint16_t addr, uint8_t v)
{
    if (addr >= 0x2000) return 0;

    if (b->in) b->in->pad1_last_bus_was_4016 = 0;
    b->open_bus_value = v;
    b->cpu_internal_bus_value = v;
    b->mem[addr & 0x07FF] = v;
    return 1;
}

void nes_bus_oam_dma_step(
    nes_bus_t *bus,
    fake6502_context *ctx
);

void nes_bus_dma_dmc_step(
    nes_bus_t *bus,
    fake6502_context *ctx
);

#ifdef __cplusplus
}
#endif

#endif