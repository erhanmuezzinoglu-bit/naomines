#include "emu_driver.h"
#include "nes_bus.h"
#include "fake6502.h"
#include "apu.h"
#include "ppu.h"
#include "exp_config.h"

extern int mapper_irq_pending(void);

static fake6502_context *g_cpu;
static nes_bus_t *g_bus;
static ppu_t *g_ppu;
static apu_t *g_apu;

int g_irq_pending = 0;
int g_nmi_pending = 0;
int g_nmi_ready = 0;
int g_irq_poll_snapshot = 0;

static uint8_t g_nmi_last_sample = 0;
static int g_cpu_reset_in_progress = 0;

/* ============================================================
 * CPU NMI LINE SAMPLING
 * ============================================================ */

void emu_driver_step_ppu_sampled(
    ppu_t *ppu,
    uint32_t cycles)
{
    if (!ppu || cycles == 0u) {
        return;
    }

    ppu_step_sampled(ppu, cycles, &g_nmi_last_sample, &g_nmi_pending);
}

/* ============================================================
 * DMC DMA CPU-CYCLE SCHEDULING
 * ============================================================ */

static void dmc_after_cpu_cycle(
    nes_bus_t *bus,
    int was_pending_before_cycle,
    int cpu_was_write)
{
    if (!bus) {
        return;
    }

    dmc_t *d = &bus->apu.dmc;

    int implicit_pending =
        (d->dma_delay_parity & 0xFEu) ==
        DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE;

    if (cpu_was_write &&
        implicit_pending &&
        d->dma_request &&
        d->dma_delay == 0) {

        d->dma_request = 0;
        d->dma_kind = APU_DMC_DMA_NONE;
        d->dma_delay = 0;
        d->dma_delay_parity = 0;
        return;
    }

    if (!was_pending_before_cycle ||
        !d->dma_request) {
        return;
    }

    if (d->dma_delay > 0) {
        d->dma_delay--;
        return;
    }

    if (cpu_was_write) {
        d->dma_delay_parity ^= 1;
    }
}

/* ============================================================
 * PPU STEPPING (NMI SAMPLED)
 * ============================================================ */

static void step_ppu_with_nmi_probe(
    nes_bus_t *bus,
    uint32_t cycles)
{
    if (!bus || !bus->ppu || cycles == 0u) {
        return;
    }

#if EXP_ABLATE
    /* M5-M7 (SADECE OLCUM): PPU adimlama tamamen kapali. */
    if (g_exp_mode >= 5) {
        return;
    }
#endif

    ppu_t *ppu = bus->ppu;

    emu_driver_step_ppu_sampled(ppu, cycles);
}

#if EXP_LAZY_PPU
/* PPU'nun bir sonraki "disariya gorunur" olayinin CPU cycle'i (mutlak). Olay,
 * o cycle'in senkronunda calisir; bir CPU cycle erken uyanma payi birakilir. */
static int g_ppu_deadline = 0;

static void lazy_ppu_sync(nes_bus_t *bus, int now)
{
    int elapsed = now - (int)bus->last_ppu_sync_tick;

    if (elapsed > 0) {
        step_ppu_with_nmi_probe(bus, (uint32_t)(elapsed * 3));
        bus->last_ppu_sync_tick = (uint32_t)now;
    }

    int ticks = (ppu_dots_to_next_event(bus->ppu) + 2) / 3 - 1;
    if (ticks < 1) ticks = 1;
#if EXP_ABLATE
    /* SADECE OLCUM: PPU adimlama kapaliyken konum donuktur, olay hesabi anlamsizdir. */
    if (g_exp_mode >= 5) ticks = 4096;
#endif
    g_ppu_deadline = now + ticks;
}
#endif

void emu_driver_sync_ppu(void)
{
    if (!g_cpu || !g_bus) {
        return;
    }

    int now = g_cpu->emu.clockticks;
    int elapsed = now - (int)g_bus->last_ppu_sync_tick;

    if (elapsed > 0) {
        step_ppu_with_nmi_probe(g_bus, (uint32_t)(elapsed * 3));
        g_bus->last_ppu_sync_tick = (uint32_t)now;
    }
}

#if EXP_LAZY_APU
/* APU'nun bir sonraki "CPU'ya gorunebilen" olayinin CPU cycle'i (mutlak). */
static int g_apu_deadline = 0;

static void lazy_apu_sync(nes_bus_t *bus, int now)
{
    int elapsed = now - (int)bus->last_apu_sync_tick;

    if (elapsed > 0) {
        apu_step(&bus->apu, elapsed);
        bus->last_apu_sync_tick = (uint32_t)now;
    }

    int ticks = apu_cycles_to_next_event(&bus->apu) - 1;
    if (ticks < 1) ticks = 1;
#if EXP_ABLATE
    /* SADECE OLCUM: APU adimi kapaliyken durum donuktur, olay hesabi anlamsizdir. */
    if (g_exp_mode == 1 || g_exp_mode >= 4) ticks = 4096;
#endif
    g_apu_deadline = now + ticks;
}
#endif

void emu_driver_sync_apu(void)
{
    if (!g_cpu || !g_bus) {
        return;
    }

    int now = g_cpu->emu.clockticks;
    int elapsed = now - (int)g_bus->last_apu_sync_tick;

    if (elapsed > 0) {
        apu_step(&g_bus->apu, elapsed);
        g_bus->last_apu_sync_tick = (uint32_t)now;
    }
}

/* ============================================================
 * CPU BUS READ
 * ============================================================ */

uint8_t fake6502_mem_read(
    fake6502_context *c,
    uint16_t address)
{
    nes_bus_t *bus =
        (nes_bus_t *)c->state_host;

    uint32_t entry_ticks =
        (uint32_t)c->emu.clockticks;

    int entry_irq = g_irq_pending;

    if (!bus) {
        c->emu.clockticks++;
        g_irq_poll_snapshot = g_irq_pending;
        return 0xFF;
    }

#if EXP_ABLATE
    /* M7 (SADECE OLCUM): surucu sarmalayicisi yok; yalniz clockticks + dogrudan okuma. */
    if (g_exp_mode == 7) {
        c->emu.clockticks++;
        bus->last_ppu_sync_tick = c->emu.clockticks;
        bus->last_apu_sync_tick = c->emu.clockticks;
        if (address < 0x2000) return bus->mem[address & 0x7FF];
        if (address >= 0x8000) return bus->mem[address];
        return nes_bus_read(bus, c, address);
    }
#endif

    int dmc_pending_before_cycle =
        bus->apu.dmc.dma_request != 0;

    int dmc_halt_now =
        dmc_pending_before_cycle &&
        bus->apu.dmc.dma_delay == 0 &&
        !g_cpu_reset_in_progress;

    if (dmc_halt_now &&
        apu_dmc_enable_blocks_halt(&bus->apu)) {
        dmc_halt_now = 0;
    }

    if (dmc_halt_now) {
        bus->last_cpu_read_addr = address;
        nes_bus_dma_dmc_step(bus, c);
#if EXP_LAZY_APU
        g_apu_deadline = 0;
#endif
    }

    c->emu.clockticks++;

#if EXP_LAZY_PPU
    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(bus, c->emu.clockticks);
    }
#else
    int elapsed_ppu =
        c->emu.clockticks -
        bus->last_ppu_sync_tick;

    if (elapsed_ppu > 0) {
        step_ppu_with_nmi_probe(
            bus,
            (uint32_t)(elapsed_ppu * 3)
        );

        bus->last_ppu_sync_tick =
            c->emu.clockticks;
    }
#endif

#if EXP_LAZY_APU
    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_apu_deadline) >= 0) {
        lazy_apu_sync(bus, c->emu.clockticks);
    }
#else
    int elapsed_apu =
        c->emu.clockticks -
        bus->last_apu_sync_tick;

    if (elapsed_apu > 0) {
        apu_step(&bus->apu, elapsed_apu);

        bus->last_apu_sync_tick =
            c->emu.clockticks;
    }
#endif

    uint8_t result;
#if EXP_ABLATE
    /* M6 (SADECE OLCUM): bus dagitimi atlanir (RAM/PRG dogrudan). */
    if (g_exp_mode == 6 && (address < 0x2000 || address >= 0x8000)) {
        result = (address < 0x2000) ? bus->mem[address & 0x7FF] : bus->mem[address];
    } else
#endif
    {
        if (!nes_bus_fast_read(bus, address, &result)) {
            result = nes_bus_read(bus, c, address);
        }
    }

#if EXP_LAZY_APU
    /* APU yazmaci/durumu okundu ($4000-$401F): bekleyen bayrak olusmus olabilir, bir
     * sonraki cycle'da senkronla ve olay hesabini tazele. */
    if (address >= 0x4000 && address <= 0x401F) {
        g_apu_deadline = c->emu.clockticks + 1;
    }
#endif

    dmc_after_cpu_cycle(
        bus,
        dmc_pending_before_cycle,
        0
    );

    if (((uint32_t)c->emu.clockticks -
         entry_ticks) <= 1u) {
        g_irq_poll_snapshot = entry_irq;
    } else {
        g_irq_poll_snapshot = g_irq_pending;
    }

    return result;
}

/* ============================================================
 * CPU BUS WRITE
 * ============================================================ */

void fake6502_mem_write(
    fake6502_context *c,
    uint16_t address,
    uint8_t val)
{
    nes_bus_t *bus =
        (nes_bus_t *)c->state_host;

    uint32_t entry_ticks =
        (uint32_t)c->emu.clockticks;

    int entry_irq = g_irq_pending;

    if (!bus) {
        c->emu.clockticks++;
        g_irq_poll_snapshot = g_irq_pending;
        return;
    }

#if EXP_ABLATE
    if (g_exp_mode == 7) {
        c->emu.clockticks++;
        bus->last_ppu_sync_tick = c->emu.clockticks;
        bus->last_apu_sync_tick = c->emu.clockticks;
        if (address < 0x2000) bus->mem[address & 0x7FF] = val;
        else nes_bus_write(bus, c, address, val);
        return;
    }
#endif

    int dmc_pending_before_cycle =
        bus->apu.dmc.dma_request != 0;

    c->emu.clockticks++;

#if EXP_LAZY_PPU
    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(bus, c->emu.clockticks);
    }
#else
    int elapsed_ppu =
        c->emu.clockticks -
        bus->last_ppu_sync_tick;

    if (elapsed_ppu > 0) {
        step_ppu_with_nmi_probe(
            bus,
            (uint32_t)(elapsed_ppu * 3)
        );

        bus->last_ppu_sync_tick =
            c->emu.clockticks;
    }
#endif

#if EXP_LAZY_APU
    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_apu_deadline) >= 0) {
        lazy_apu_sync(bus, c->emu.clockticks);
    }
#else
    int elapsed_apu =
        c->emu.clockticks -
        bus->last_apu_sync_tick;

    if (elapsed_apu > 0) {
        apu_step(&bus->apu, elapsed_apu);

        bus->last_apu_sync_tick =
            c->emu.clockticks;
    }
#endif

#if EXP_ABLATE
    if (g_exp_mode == 6 && address < 0x2000) {
        bus->mem[address & 0x7FF] = val;
    } else
#endif
    {
        if (!nes_bus_fast_write(bus, address, val)) {
            nes_bus_write(bus, c, address, val);
        }
    }

#if EXP_LAZY_APU
    if (address >= 0x4000 && address <= 0x401F) {
        g_apu_deadline = c->emu.clockticks + 1;
    }
#endif

#if EXP_LAZY_PPU
    /* PPU yazmaci yazildi (ornegin $2000 ile NMI acildi): olasi yukselen kenar, eager
     * surucuyle ayni cycle'da yakalansin diye bir sonraki cycle'da senkronla. */
    if (address >= 0x2000 && address <= 0x3FFF) {
        g_ppu_deadline = c->emu.clockticks + 1;
    }
#endif

    dmc_after_cpu_cycle(
        bus,
        dmc_pending_before_cycle,
        1
    );

    if (((uint32_t)c->emu.clockticks -
         entry_ticks) <= 1u) {
        g_irq_poll_snapshot = entry_irq;
    } else {
        g_irq_poll_snapshot = g_irq_pending;
    }
}

/* ============================================================
 * INIT / RESET
 * ============================================================ */

void emu_driver_init(
    fake6502_context *cpu,
    ppu_t *ppu,
    nes_input_t *in,
    nes_bus_t *bus)
{
    (void)in;

    g_cpu = cpu;
    g_bus = bus;
    g_ppu = ppu;
    g_apu = &bus->apu;

    g_cpu->state_host = (void *)bus;

    g_irq_poll_snapshot = 0;
    g_nmi_pending = 0;
    g_nmi_ready = 0;
    g_nmi_last_sample = 0;
#if EXP_LAZY_PPU
    g_ppu_deadline = 0;
#endif
#if EXP_LAZY_APU
    g_apu_deadline = 0;
#endif
}

void emu_driver_reset_from_vector(void)
{
    if (!g_cpu) {
        return;
    }

    /* Tembel APU: sifirlamadan once bekleyen cycle'lari calistir (eager davranisla ayni). */
    emu_driver_sync_apu();

    int warm_reset =
        g_cpu->emu.clockticks != 0 ||
        g_cpu->emu.instructions != 0;

    if (warm_reset && g_ppu) {
        g_ppu->reg_ctrl = 0x00;
    }

    g_cpu->emu.pending_irq = 0;
    g_cpu->emu.nmi_defer_after_interrupt = 0;

    g_irq_pending = 0;
    g_nmi_pending = 0;
    g_nmi_ready = 0;
    g_nmi_last_sample = 0;
    g_irq_poll_snapshot = 0;

    if (g_bus) {
        g_bus->oam_dma_pending = 0;
        g_bus->oam_dma_page = 0;

        g_bus->apu.dmc.dma_request = 0;
        g_bus->apu.dmc.dma_kind =
            APU_DMC_DMA_NONE;
        g_bus->apu.dmc.dma_delay = 0;
        g_bus->apu.dmc.dma_delay_parity = 0;
    }

    if (g_apu) {
        apu_reset(g_apu);
    }

#if EXP_LAZY_PPU
    g_ppu_deadline = 0;
#endif
#if EXP_LAZY_APU
    g_apu_deadline = 0;
#endif
    g_cpu_reset_in_progress = 1;
    fake6502_reset(g_cpu);
    g_cpu_reset_in_progress = 0;

    emu_driver_sync_ppu();

    g_nmi_pending = 0;
    g_nmi_ready = 0;
    g_nmi_last_sample =
        ppu_nmi_line(g_ppu) ? 1u : 0u;
}

/* ============================================================
 * INTERRUPT REQUESTS
 * ============================================================ */

void emu_driver_request_irq(void)
{
    if (!g_irq_pending) {
        g_irq_pending = 1;
    }
}

void emu_driver_clear_irq(void)
{
    int apu_active    = g_apu ? apu_irq_line(g_apu) : 0;
    int mapper_active = mapper_irq_pending();

    g_irq_pending = (apu_active || mapper_active) ? 1 : 0;
}

/* ============================================================
 * STEP
 * ============================================================ */

uint32_t emu_driver_step_1(void)
{
    if (!g_cpu) {
        return 0;
    }

    if (g_nmi_ready &&
        !g_cpu->emu.nmi_defer_after_interrupt) {

        g_cpu->emu.pending_irq = 0;

        fake6502_nmi(g_cpu);

        emu_driver_sync_ppu();
        return 7;
    }

    int ticks_before = g_cpu->emu.clockticks;

    fake6502_step(g_cpu);

    if (g_bus->oam_dma_pending) {
        emu_driver_sync_ppu();
        nes_bus_oam_dma_step(g_bus, g_cpu);
#if EXP_LAZY_APU
        g_apu_deadline = 0;
#endif
    }

    int ticks_after = g_cpu->emu.clockticks;
    int cpu_cycles = ticks_after - ticks_before;

    int final_ppu_sync =
        g_cpu->emu.clockticks -
        g_bus->last_ppu_sync_tick;

    if (final_ppu_sync > 0) {
        step_ppu_with_nmi_probe(
            g_bus,
            (uint32_t)(final_ppu_sync * 3)
        );

        g_bus->last_ppu_sync_tick =
            g_cpu->emu.clockticks;
    }

    if (g_nmi_ready &&
        !g_cpu->emu.nmi_defer_after_interrupt) {

        g_cpu->emu.pending_irq = 0;

        fake6502_nmi(g_cpu);

        emu_driver_sync_ppu();
        return cpu_cycles;
    }

    if (g_cpu->emu.pending_irq &&
        !g_nmi_ready) {
        g_cpu->emu.pending_irq = 0;

        fake6502_irq(g_cpu);

        emu_driver_sync_ppu();
        return 7;
    }

    if (g_cpu->emu.nmi_defer_after_interrupt &&
        g_cpu->emu.opcode != 0x00 &&
        g_nmi_ready) {
        g_cpu->emu.nmi_defer_after_interrupt = 0;

        g_cpu->emu.pending_irq = 0;

        fake6502_nmi(g_cpu);
    }

#if !EXP_LAZY_APU
    int elapsed_apu =
        ticks_after -
        g_bus->last_apu_sync_tick;

    if (elapsed_apu > 0) {
        apu_step(g_apu, elapsed_apu);
        g_bus->last_apu_sync_tick =
            ticks_after;
    }
#endif

    emu_driver_sync_ppu();
    return cpu_cycles;
}