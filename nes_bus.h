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
/* mapper.c (PRG-RAM acik/yazma serbest): nes_bus.h mapper.h'yi dahil edemez (dongusel), burada bildirilir. */
int mapper_prg_ram_enabled(const nes_bus_t *bus);
int mapper_prg_ram_write_enabled(const nes_bus_t *bus);
/* mapper.c: 1 iken PRG-RAM'in etkinligi PPU'nun o anki konumuna bagli (MMC1 SNROM, R1/R2 bit 4 farkli): PRG-RAM erisimleri hizli
 * yoldan degil, erisim anina PPU senkronlu yavas yoldan yapilir (nes_bus_read/write). */
extern int g_prgram_ppu_dep;
/* nes_rom.c: CPU $8000-$FFFF gorunumu; nes_prg_img icerigi pencerelere esittir (bank degisimi goruntuyu degistirir). */
extern const uint8_t *nes_prg_page[4];
extern const uint8_t *nes_prg_img;
#include "exp_config.h"
#ifndef EXP_PRG_PTR   /* eski bir exp_config.h ile de varsayilan acik */
#define EXP_PRG_PTR 1
#endif
#if EXP_PRG_PTR
#define NES_PRG_READ(bus_, a) (nes_prg_img[(a)])
#else
#define NES_PRG_READ(bus_, a) ((bus_)->mem[(a)])
#endif
/* Yan etkisiz okuma (tani/HUD): RAM ve PRG; digerleri mem[] (eski davranis). */
static inline uint8_t nes_bus_peek(const nes_bus_t *b, uint16_t addr)
{
    if (addr < 0x2000) return b->mem[addr & 0x07FF];
    if (addr >= 0x8000) return NES_PRG_READ(b, addr);
    return b->mem[addr];
}

static inline int nes_bus_fast_read(nes_bus_t *b, uint16_t addr, uint8_t *out)
{
    uint8_t v;

    if (addr < 0x2000)       v = b->mem[addr & 0x07FF];
    else if (addr >= 0x8000) v = NES_PRG_READ(b, addr);
    else if (addr >= 0x6000) { if (g_prgram_ppu_dep) return 0; v = mapper_prg_ram_enabled(b) ? b->cart_sram[addr - 0x6000] : b->open_bus_value; }   /* PRG-RAM */
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
    if (addr >= 0x2000) {
        /* PRG-RAM ($6000-$7FFF): mapper'a gitmez, yalniz cart_sram'e yazilir; PPU'yu senkronlamak gerekmez
         * (nes_bus_write bu alan icin de senkronluyordu: gereksiz, PPU durumu zamanin saf fonksiyonu). */
        if (addr >= 0x6000 && addr < 0x8000 && !g_prgram_ppu_dep) {
            if (b->in) b->in->pad1_last_bus_was_4016 = 0;
            b->open_bus_value = v;
            b->cpu_internal_bus_value = v;
            if (mapper_prg_ram_write_enabled(b)) b->cart_sram[addr - 0x6000] = v;
            return 1;
        }
        return 0;
    }

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