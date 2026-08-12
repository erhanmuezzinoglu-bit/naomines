#include "emu_driver.h"
#include "nes_bus.h"
#include "fake6502.h"
#include "apu.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

extern int g_nmi_pending;

// ============================================================
// PHASE 1: PER-CYCLE CLOCK ADVANCEMENT
// ============================================================

uint8_t fake6502_mem_read(fake6502_context *c, uint16_t address) {
    nes_bus_t *bus = (nes_bus_t *)c->state_host;
    if (!bus) {
        c->emu.clockticks++;
        return 0xFF;
    }
    c->emu.clockticks++;

    int elapsed_ppu = c->emu.clockticks - bus->last_ppu_sync_tick;
    if (elapsed_ppu > 0) {
        ppu_step_cycles(bus->ppu, elapsed_ppu * 3);
        bus->last_ppu_sync_tick = c->emu.clockticks;
    if (bus->ppu->nmi_requested) {
            g_nmi_pending = 1;
            bus->ppu->nmi_requested = 0;
        }
    }

    // APU Per-Cycle Sync (12-1 Latency ve 14-3/4 Fix)
    int elapsed_apu = c->emu.clockticks - bus->last_apu_sync_tick;
    if (elapsed_apu > 0) {
        apu_step(&bus->apu, elapsed_apu);
        bus->last_apu_sync_tick = c->emu.clockticks;
    if (bus->ppu->nmi_requested) {
            g_nmi_pending = 1;
            bus->ppu->nmi_requested = 0;
        }
    }

    // DMC DMA DUMMY READ/COLLISION LOGIC
    // KRİTİK: DMA tetiklenmeden ÖNCE last_cpu_read_addr'i BU read'in adresine
    // ayarla. Aksi halde halt/alignment cycle'ları önceki komutun adresinden
    // okur (blargg dma_2007_read testi satır 3 'v' register'ını ilerletmez).
    if (bus->apu.dmc.dma_request) {
        bus->last_cpu_read_addr = address;
        nes_bus_dma_dmc_step(bus, c);
    }

    return nes_bus_read(bus, c, address);
}

void fake6502_mem_write(fake6502_context *c, uint16_t address, uint8_t val) {
    nes_bus_t *bus = (nes_bus_t *)c->state_host;
    if (!bus) {
        c->emu.clockticks++;
        return;
    }
    c->emu.clockticks++;

    int elapsed_ppu = c->emu.clockticks - bus->last_ppu_sync_tick;
    if (elapsed_ppu > 0) {
        ppu_step_cycles(bus->ppu, elapsed_ppu * 3);
        bus->last_ppu_sync_tick = c->emu.clockticks;
    }

    // APU Per-Cycle Sync (12-1 Latency ve 14-3/4 Fix)
    nes_bus_write(bus, c, address, val);
    int elapsed_apu = c->emu.clockticks - bus->last_apu_sync_tick;
    if (elapsed_apu > 0) {
        apu_step(&bus->apu, elapsed_apu);
        bus->last_apu_sync_tick = c->emu.clockticks;
    }

    // DMA + $2007 Write: write cycle sırasında DMC DMA bekleniyorsa 1 alignment cycle ekle.
    // Bu, DMA'nın write cycle'ı tamamlamasını beklemesini simüle eder.
    if (address == 0x2007 && bus->apu.dmc.dma_request) {
        c->emu.clockticks++;
        ppu_step_cycles(bus->ppu, 3);
        bus->last_ppu_sync_tick = c->emu.clockticks;
        apu_step(&bus->apu, 1);
        bus->last_apu_sync_tick = c->emu.clockticks;
    }

}

// --- Global CPU, APU ve Trace Durumları ---
static fake6502_context *g_cpu;
static nes_bus_t *g_bus;
static ppu_t *g_ppu;
static apu_t *g_apu;
static trace_state_t *g_tr;
int g_irq_pending = 0;
int g_nmi_pending = 0;
static uint32_t g_instr_count = 0;
static int g_nmi_delay = 0;

void emu_driver_init(fake6502_context *cpu,
                     ppu_t *ppu,
                     nes_input_t *in,
                     trace_state_t *tr,
                     nes_bus_t *bus) {
    g_cpu = cpu;
    g_bus = bus;
    g_ppu = ppu;
    g_apu = &bus->apu;
    g_tr = tr;
    g_cpu->state_host = (void *)bus;
}

void emu_driver_reset_from_vector(void) {
    // ÖNCE reset fonksiyonunu çağır ki CPU içeride clockticks'e bakıp 
    // bunun bir Warm Reset olduğunu anlayabilsin.
    fake6502_reset(g_cpu);

    if (g_bus) {
        g_bus->last_apu_sync_tick = g_cpu->emu.clockticks;
        g_bus->last_apu_sync_tick = g_cpu->emu.clockticks;
    }

    if (g_apu) {
        apu_reset(g_apu);
    }

    g_irq_pending = 0;
    g_nmi_delay = 0;
    g_instr_count = 0;

    if (g_ppu) {
        g_ppu->nmi_requested = 0;
    }
}

void emu_driver_request_irq(void) {
    if (!g_irq_pending) {
        g_irq_pending = 1;
    }
}

void emu_driver_clear_irq(void) {
    g_irq_pending = 0;
}

void emu_driver_request_nmi_delayed(void) {
    g_nmi_delay = 2;
}

uint32_t emu_driver_step_1(void) {
    if (!g_cpu) {
        return 0;
    }

    // DMC Cycle Stealing
    if (g_apu->dmc.dma_request) {
        nes_bus_dma_dmc_step(g_bus, g_cpu);
    }

    int ticks_before = g_cpu->emu.clockticks;
    fake6502_step(g_cpu);
    int ticks_after = g_cpu->emu.clockticks;
    int cpu_cycles = ticks_after - ticks_before;

    // Geri kalan (read/write yapılmayan) cycle'lar için catch-up
    int final_ppu_sync = g_cpu->emu.clockticks - g_bus->last_ppu_sync_tick;
    if (final_ppu_sync > 0) {
        ppu_step_cycles(g_bus->ppu, final_ppu_sync * 3);
        g_bus->last_ppu_sync_tick = g_cpu->emu.clockticks;
    if (g_ppu->nmi_requested) {         // ← YENİ
            g_nmi_pending = 1;
            g_ppu->nmi_requested = 0;
        }
    }

    if (g_nmi_delay > 0) {
        g_nmi_delay--;
        if (g_nmi_delay == 0) {
            g_nmi_pending = 1;              // ← DEĞİŞTİ: g_ppu->nmi_requested yerine
        }
    }

    if (g_ppu->nmi_requested) {
        g_nmi_pending = 1;
        g_ppu->nmi_requested = 0;
    }

    // g_nmi_pending = 0 ise brk() bu NMI'ı zaten tüketti → double-fire yok
    if (g_nmi_pending) {
        fake6502_nmi(g_cpu);
        g_nmi_pending = 0;
        g_ppu->dbg_nmi_sent++;
    }

    // Geri kalan cycle'lar için catch-up
    int elapsed_apu = ticks_after - g_bus->last_apu_sync_tick;
    if (elapsed_apu > 0) {
        apu_step(g_apu, elapsed_apu);
        g_bus->last_apu_sync_tick = ticks_after;
    }

    g_instr_count++;
    return cpu_cycles;
}

uint32_t emu_driver_instr_count(void) { return g_instr_count; }
void emu_driver_instr_count_reset(void) { g_instr_count = 0; }

int emu_driver_clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}