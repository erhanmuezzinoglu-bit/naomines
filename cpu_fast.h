#ifndef CPU_FAST_H
#define CPU_FAST_H

/* Cekirdege (fake6502.c) gomulu bus hizli yolu.
 *
 * fake6502_mem_read/write her bus cycle'inda ayri bir fonksiyon cagrisiydi. Asagidaki satir ici
 * yollar, emu_driver.c'deki fake6502_mem_read/write'in "hicbir sey beklemiyor" durumundaki
 * davranisini AYNEN yapar; kosullardan biri tutmazsa gercek (yavas) fonksiyon cagrilir.
 *
 * Hizli yol kosullari (okuma):  bus var, adres RAM ($0000-$1FFF), PRG-RAM ($6000-$7FFF) veya PRG ($8000-$FFFF),
 *   DMC DMA istegi yok, bir sonraki PPU/APU deadline'ina ulasilmadi.
 * Hizli yol kosullari (yazma):  ayni, ama yalniz RAM ($0000-$1FFF) ve PRG-RAM ($6000-$7FFF) yazimi.
 * Bu kosullarda yavas yolun yan etkileri: clockticks++, nes_bus_fast_read/write'in alan atamalari,
 * g_irq_poll_snapshot = g_irq_pending (PPU/APU senkronu olmadigi icin entry_irq == g_irq_pending);
 * dmc_after_cpu_cycle bir sey yapmaz (dma_request == 0). */

#include <stdint.h>
#include "fake6502.h"
#include "nes_bus.h"
#include "exp_config.h"


/* mapper.c: PRG-RAM acik / yazma serbest. nes_bus.h ayni prototipleri de bildirir (ayni imza, tekrar bildirim serbest);
 * burada tekrar bildirmemizin nedeni, cpu_fast.h'in eski bir nes_bus.h ile de derlenebilmesi. */
int mapper_prg_ram_enabled(const nes_bus_t *bus);
int mapper_prg_ram_write_enabled(const nes_bus_t *bus);

extern int g_irq_pending;
extern int g_irq_poll_snapshot;
extern int g_dl_min;
extern unsigned g_write_epoch;   /* her RAM yazimi / yavas bus erisimi artirir (bos dongu tespiti) */

#define CPU_FAST_ABLATE_M7() 0
#define CPU_FAST_ABLATE_M6() 0

#define CPU_FAST_TRACE(c, a, v, r) ((void)0)

static inline uint8_t cpu_read_fast(fake6502_context *c, uint16_t address)
{
    nes_bus_t *bus = (nes_bus_t *)c->state_host;
    const int now = c->emu.clockticks + 1;


    if (__builtin_expect(
            bus != 0 &&
            bus->apu.dmc.dma_request == 0 &&
            (int32_t)((uint32_t)now - (uint32_t)g_dl_min) < 0 &&
            !CPU_FAST_ABLATE_M7(), 1)) {

        uint8_t v;
        if (__builtin_expect((uint16_t)(address - 0x2000u) >= 0x6000u, 1)) {   /* RAM ($0000-$1FFF) veya PRG ($8000-$FFFF) */
            v = (address < 0x2000u) ? bus->mem[address & 0x07FFu] : bus->mem[address];
        } else if ((uint16_t)(address - 0x6000u) < 0x2000u) {                  /* PRG-RAM ($6000-$7FFF) */
            v = mapper_prg_ram_enabled(bus) ? bus->cart_sram[address - 0x6000u] : bus->open_bus_value;
            g_write_epoch++;   /* yavas yol her okumada artirirdi: bos dongu tespiti degismesin */
        } else {
            return (fake6502_mem_read)(c, address);                            /* PPU/APU/kartus yazmaclari */
        }

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


    if (__builtin_expect(
            bus != 0 &&
            (address < 0x2000u || (uint16_t)(address - 0x6000u) < 0x2000u) &&   /* RAM veya PRG-RAM */
            bus->apu.dmc.dma_request == 0 &&
            (int32_t)((uint32_t)now - (uint32_t)g_dl_min) < 0 &&
            !CPU_FAST_ABLATE_M7(), 1)) {

        if (address >= 0x2000u) {   /* PRG-RAM ($6000-$7FFF): mapper'a gitmez, PPU senkronu gerekmez */
            c->emu.clockticks = now;
            g_write_epoch++;
            if (!CPU_FAST_ABLATE_M6()) {
                if (bus->in) bus->in->pad1_last_bus_was_4016 = 0;
                bus->open_bus_value = val;
                bus->cpu_internal_bus_value = val;
            }
            if (mapper_prg_ram_write_enabled(bus)) bus->cart_sram[address - 0x6000u] = val;
            g_irq_poll_snapshot = g_irq_pending;
            CPU_FAST_TRACE(c, address, val, 0);
            return;
        }

        c->emu.clockticks = now;
        g_write_epoch++;
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


#endif
