#ifndef CPU_FAST_H
#define CPU_FAST_H

/* Cekirdege (fake6502.c) gomulu bus hizli yolu.
 *
 * fake6502_mem_read/write her bus cycle'inda ayri bir fonksiyon cagrisiydi. Asagidaki satir ici
 * yollar, emu_driver.c'deki fake6502_mem_read/write'in "hicbir sey beklemiyor" durumundaki
 * davranisini AYNEN yapar; kosullardan biri tutmazsa gercek (yavas) fonksiyon cagrilir.
 *
 * Hizli yol kosullari (okuma):  bus var, adres RAM ($0000-$1FFF) veya PRG ($8000-$FFFF),
 *   DMC DMA istegi yok, bir sonraki PPU/APU deadline'ina ulasilmadi.
 * Hizli yol kosullari (yazma):  ayni, ama yalniz RAM yazimi ($0000-$1FFF).
 * Bu kosullarda yavas yolun yan etkileri: clockticks++, nes_bus_fast_read/write'in alan atamalari,
 * g_irq_poll_snapshot = g_irq_pending (PPU/APU senkronu olmadigi icin entry_irq == g_irq_pending);
 * dmc_after_cpu_cycle bir sey yapmaz (dma_request == 0). */

#include <stdint.h>
#include "fake6502.h"
#include "nes_bus.h"
#include "exp_config.h"

#if EXP_LAZY_PPU && EXP_LAZY_APU

extern int g_irq_pending;
extern int g_irq_poll_snapshot;
extern int g_dl_min;
#if EXP_IDLE_FF
extern unsigned g_write_epoch;   /* her RAM yazimi / yavas bus erisimi artirir (bos dongu tespiti) */
#endif

#if EXP_ABLATE
/* SADECE OLCUM. M7 (surucu sarmalayicisi yok) ve M6 (bus alan atamalari yok) asagida satir ici
 * taklit edilir; boylece M0-M5 ve M8/M9 hizli yolu, M6/M7 ise yalnizca kendi atladiklari isi kaldirilmis
 * halini olcer (eskiden mod >= 6 yavas yola dusuyordu: M6-M9 satirlari sisiyordu). */
#define CPU_FAST_ABLATE_M7() (g_exp_mode == 7)
#define CPU_FAST_ABLATE_M6() (g_exp_mode == 6)
#else
#define CPU_FAST_ABLATE_M7() 0
#define CPU_FAST_ABLATE_M6() 0
#endif

#ifdef CPU_FAST_HOOK
/* SADECE TEST: harness, hizli yoldaki bus islemlerini de izleyebilsin. Uretim derlemesinde tanimli degil. */
extern void cpu_fast_hook(fake6502_context *c, uint16_t address, uint8_t v, int isread);
#define CPU_FAST_TRACE(c, a, v, r) cpu_fast_hook((c), (a), (v), (r))
#else
#define CPU_FAST_TRACE(c, a, v, r) ((void)0)
#endif

static inline uint8_t cpu_read_fast(fake6502_context *c, uint16_t address)
{
    nes_bus_t *bus = (nes_bus_t *)c->state_host;
    const int now = c->emu.clockticks + 1;

#if EXP_ABLATE
    if (CPU_FAST_ABLATE_M7() && bus != 0 && (address < 0x2000u || address >= 0x8000u)) {
        c->emu.clockticks = now;
        bus->last_ppu_sync_tick = (uint32_t)now;
        bus->last_apu_sync_tick = (uint32_t)now;
        {
            const uint8_t v7 = (address < 0x2000u) ? bus->mem[address & 0x07FFu] : bus->mem[address];
            CPU_FAST_TRACE(c, address, v7, 1);
            return v7;
        }
    }
#endif

    if (__builtin_expect(
            bus != 0 &&
            (uint16_t)(address - 0x2000u) >= 0x6000u &&   /* RAM ($0000-$1FFF) veya PRG ($8000-$FFFF) */
            bus->apu.dmc.dma_request == 0 &&
            (int32_t)((uint32_t)now - (uint32_t)g_dl_min) < 0 &&
            !CPU_FAST_ABLATE_M7(), 1)) {

        const uint8_t v = (address < 0x2000u) ? bus->mem[address & 0x07FFu] : bus->mem[address];

        c->emu.clockticks = now;
        if (!CPU_FAST_ABLATE_M6()) {
            bus->last_cpu_read_addr = address;
            if (bus->in) bus->in->pad1_last_bus_was_4016 = 0;
            bus->open_bus_value = v;
            bus->cpu_internal_bus_value = v;
        }
        g_irq_poll_snapshot = g_irq_pending;
        CPU_FAST_TRACE(c, address, v, 1);
        return v;
    }

    return (fake6502_mem_read)(c, address);
}

static inline void cpu_write_fast(fake6502_context *c, uint16_t address, uint8_t val)
{
    nes_bus_t *bus = (nes_bus_t *)c->state_host;
    const int now = c->emu.clockticks + 1;

#if EXP_ABLATE
    if (CPU_FAST_ABLATE_M7() && bus != 0 && address < 0x2000u) {
        c->emu.clockticks = now;
        bus->last_ppu_sync_tick = (uint32_t)now;
        bus->last_apu_sync_tick = (uint32_t)now;
        bus->mem[address & 0x07FFu] = val;
        CPU_FAST_TRACE(c, address, val, 0);
        return;
    }
#endif

    if (__builtin_expect(
            bus != 0 &&
            address < 0x2000u &&
            bus->apu.dmc.dma_request == 0 &&
            (int32_t)((uint32_t)now - (uint32_t)g_dl_min) < 0 &&
            !CPU_FAST_ABLATE_M7(), 1)) {

        c->emu.clockticks = now;
#if EXP_IDLE_FF
        g_write_epoch++;
#endif
        if (!CPU_FAST_ABLATE_M6()) {
            if (bus->in) bus->in->pad1_last_bus_was_4016 = 0;
            bus->open_bus_value = val;
            bus->cpu_internal_bus_value = val;
        }
        bus->mem[address & 0x07FFu] = val;
        g_irq_poll_snapshot = g_irq_pending;
        CPU_FAST_TRACE(c, address, val, 0);
        return;
    }

    (fake6502_mem_write)(c, address, val);
}

#define fake6502_mem_read(c, a)       cpu_read_fast((c), (a))
#define fake6502_mem_write(c, a, v)   cpu_write_fast((c), (a), (v))

#endif /* EXP_LAZY_PPU && EXP_LAZY_APU */

#endif