#include "emu_driver.h"
#include "nes_bus.h"
#include "fake6502.h"
#include "apu.h"
#include "ppu.h"
#include "exp_config.h"
#include <string.h>
#include <stddef.h>

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

    /* Asagidaki iki kol da d->dma_request != 0 gerektirir: DMC istegi yokken hicbir sey yapmaz. */
    if (!d->dma_request) {
        return;
    }

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


    ppu_t *ppu = bus->ppu;

    emu_driver_step_ppu_sampled(ppu, cycles);
}

/* Tembel senkron deadline'lari (mutlak CPU cycle). cpu_fast.h (cekirdege gomulu hizli bus yolu)
 * ikisini tek karsilastirmayla kontrol etmek icin g_dl_min = min(ppu, apu) tutar; deadline'i
 * degistiren HER atama DL_REFRESH() ile biter. */
int g_ppu_deadline = 0;
int g_apu_deadline = 0;

/* Kare butcesi / gozlem siniri: coklu komut hizli yolu (fake6502_run_fast) clockticks >= g_run_stop olunca durur; 0 = tek komut.
 * emu_driver_ff_set_stop ile ayarlanir (bos dongu atlamasindan bagimsiz). */
static int g_run_stop = 0;

unsigned g_write_epoch = 0;          /* cpu_fast.h ile paylasilir */
int g_ff_stop = 0;                   /* mutlak clockticks siniri; 0 = atlama kapali (fake6502.c hizli yolu da okur) */
unsigned long g_ff_cycles_total = 0; /* istatistik (HUD/test) */
unsigned long g_ff_events_total = 0;
void emu_driver_ff_set_stop(int abs_ticks) { g_ff_stop = abs_ticks; g_run_stop = abs_ticks; }
static struct {
    int valid;
    uint32_t w0;          /* a | x<<8 | y<<16 | flags<<24 (fake6502_cpu_state ilk 4 bayti) */
    uint8_t s;
    uint16_t pc;
    unsigned epoch;
    int ticks;
    int instrs;
} g_ff_lp;
int g_dl_min = 0;
#define DL_REFRESH() (g_dl_min = ((int32_t)((uint32_t)g_ppu_deadline - (uint32_t)g_apu_deadline) < 0) ? g_ppu_deadline : g_apu_deadline)

/* PPU'nun bir sonraki "disariya gorunur" olayinin CPU cycle'i (mutlak). Olay,
 * o cycle'in senkronunda calisir; bir CPU cycle erken uyanma payi birakilir. */

static void lazy_ppu_sync(nes_bus_t *bus, int now)
{
    int elapsed = now - (int)bus->last_ppu_sync_tick;

    if (elapsed > 0) {
        step_ppu_with_nmi_probe(bus, (uint32_t)(elapsed * 3));
        bus->last_ppu_sync_tick = (uint32_t)now;
    }

    int ticks = (ppu_dots_to_next_event(bus->ppu) + 2) / 3 - 1;
    if (ticks < 1) ticks = 1;
    g_ppu_deadline = now + ticks; DL_REFRESH();
}

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

/* Komut sonu / donus noktasi: deadline'a ulasildiysa (bir sonraki bus cycle'inda
 * zaten senkron gerekecekti) PPU'yu simdi senkronla ve deadline'i yenile. */
static inline void ppu_sync_if_due(void)
{
    if ((int32_t)((uint32_t)g_cpu->emu.clockticks - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(g_bus, g_cpu->emu.clockticks);
    }
}

/* APU'nun bir sonraki "CPU'ya gorunebilen" olayinin CPU cycle'i (mutlak). */

static void lazy_apu_sync(nes_bus_t *bus, int now)
{
    int elapsed = now - (int)bus->last_apu_sync_tick;

    if (elapsed > 0) {
        apu_step(&bus->apu, elapsed);
        bus->last_apu_sync_tick = (uint32_t)now;
    }

    int ticks = apu_cycles_to_next_event(&bus->apu) - 1;
    if (ticks < 1) ticks = 1;
    g_apu_deadline = now + ticks; DL_REFRESH();
}

/* DMA'da calinan bir cycle'in sonunda (clockticks artirildi): bus cycle'indaki gibi yalniz
 * deadline'i gelen tembel bilesenleri senkronla. Tembel olmayan bilesenler nes_bus.c'de
 * cycle basina senkronlanir. */
void emu_driver_dma_cycle_events(void)
{
    const int now = g_cpu->emu.clockticks;
    if ((int32_t)((uint32_t)now - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(g_bus, now);
    }
    if ((int32_t)((uint32_t)now - (uint32_t)g_apu_deadline) >= 0) {
        lazy_apu_sync(g_bus, now);
    }
}

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
    g_write_epoch++;
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
        g_apu_deadline = 0; DL_REFRESH();
    }

    c->emu.clockticks++;

    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(bus, c->emu.clockticks);
    }

    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_apu_deadline) >= 0) {
        lazy_apu_sync(bus, c->emu.clockticks);
    }

    uint8_t result;
    {
        if (!nes_bus_fast_read(bus, address, &result)) {
            result = nes_bus_read(bus, c, address);
        }
    }

    /* APU yazmaci/durumu okundu ($4000-$401F): bekleyen bayrak olusmus olabilir, bir
     * sonraki cycle'da senkronla ve olay hesabini tazele. */
    if (address >= 0x4000 && address <= 0x401F) {
        g_apu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }
    /* $2007 okumasi v'yi artirir (A12 yukselirse mapper saatlenir): IRQ'ya kalan saat sayisi degisebilir. */
    if (address >= 0x2000 && address <= 0x3FFF && (address & 7u) == 7u) {
        g_ppu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }

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
    g_write_epoch++;
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


    int dmc_pending_before_cycle =
        bus->apu.dmc.dma_request != 0;

    c->emu.clockticks++;

    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_ppu_deadline) >= 0) {
        lazy_ppu_sync(bus, c->emu.clockticks);
    }

    if ((int32_t)((uint32_t)c->emu.clockticks - (uint32_t)g_apu_deadline) >= 0) {
        lazy_apu_sync(bus, c->emu.clockticks);
    }

    {
        if (!nes_bus_fast_write(bus, address, val)) {
            nes_bus_write(bus, c, address, val);
        }
    }

    if (address >= 0x4000 && address <= 0x401F) {
        g_apu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }

    /* PPU yazmaci yazildi (ornegin $2000 ile NMI acildi): olasi yukselen kenar, eager
     * surucuyle ayni cycle'da yakalansin diye bir sonraki cycle'da senkronla. */
    /* + mapper yazmaclari ($4020-$5FFF, $8000+): MMC3 IRQ sayaci/latch/etkin degisir, siradaki IRQ'ya kalan A12 saati
     * (ppu_dots_to_next_event) degisir: esik, yazmadan ONCEKI duruma gore hesaplanmisti. PRG-RAM'e yazma mapper'a gitmez. */
    if ((address >= 0x2000 && address <= 0x3FFF) ||
        (address >= 0x4020 && (address < 0x6000 || address >= 0x8000))) {
        g_ppu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }

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
    g_ppu_deadline = 0; DL_REFRESH();
    g_apu_deadline = 0; DL_REFRESH();
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

    g_ppu_deadline = 0; DL_REFRESH();
    g_apu_deadline = 0; DL_REFRESH();
    g_write_epoch++;
    g_ff_lp.valid = 0;
    g_ff_stop = 0;
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

/* ---- Bos dongu atlama (bkz. exp_config.h EXP_IDLE_FF) ----
 * Gecerlilik: son iki geri-dallanma anindaki CPU durumu (pc,a,x,y,s,flags) AYNI ve aralarinda hicbir
 * RAM yazimi / yavas bus erisimi (g_write_epoch) olmadiysa tur, bellek ve yazmaclar degismeden tekrar
 * eder. Tur icindeki her bus cycle'i hizli yoldaydi (deadline gelmedi, DMC istegi yok), yani tur
 * disariya gorunur hicbir sey yapmadi. Bir sonraki gorunur olay g_dl_min deadline'inda (PPU/APU) olur;
 * ondan once biten tum turlar atlanabilir. NMI/IRQ bekleyen ya da erteleme durumu varsa atlanmaz. */

_Static_assert(offsetof(fake6502_cpu_state, a) == 0 && offsetof(fake6502_cpu_state, x) == 1 &&
               offsetof(fake6502_cpu_state, y) == 2 && offsetof(fake6502_cpu_state, flags) == 3,
               "idle_ff_candidate: a,x,y,flags ardisik 4 bayt olmali");

static inline __attribute__((always_inline)) int idle_ff_candidate(void)
{
    fake6502_context *const c = g_cpu;
    int skipped = 0;
    uint32_t w0;
    memcpy(&w0, &c->cpu.a, sizeof w0);   /* a, x, y, flags: yapi basinda ardisik 4 bayt */

    if (g_ff_lp.valid &&
        g_ff_lp.pc == c->cpu.pc && g_ff_lp.s == c->cpu.s && g_ff_lp.w0 == w0 &&
        g_ff_lp.epoch == g_write_epoch &&
        !g_nmi_pending && !g_nmi_ready && !c->emu.pending_irq &&
        !c->emu.nmi_defer_after_interrupt && !g_cpu_reset_in_progress &&
        g_bus->apu.dmc.dma_request == 0) {

        const int K = c->emu.clockticks - g_ff_lp.ticks;
        const int L = c->emu.instructions - g_ff_lp.instrs;

        if (K > 0 && L > 0) {
            const int now = c->emu.clockticks;
            /* Atlanan turlarin SON cycle'i da deadline'dan once olmali (hizli yol: now+1 < g_dl_min). */
            int avail_dl   = (int)((uint32_t)g_dl_min - (uint32_t)now) - 1;
            int avail_stop = (int)((uint32_t)g_ff_stop - (uint32_t)now);
            int avail = avail_dl < avail_stop ? avail_dl : avail_stop;

            if (avail >= K) {
                const int N = avail / K;
                c->emu.clockticks    += N * K;
                c->emu.instructions  += N * L;
                skipped = N * K;
                g_ff_cycles_total += (unsigned long)skipped;
                g_ff_events_total++;
            }
        }
    } else {
        g_ff_lp.valid = 1;
        g_ff_lp.w0    = w0;
        g_ff_lp.s     = c->cpu.s;
        g_ff_lp.pc    = c->cpu.pc;
        g_ff_lp.epoch = g_write_epoch;
    }

    g_ff_lp.ticks  = c->emu.clockticks;
    g_ff_lp.instrs = c->emu.instructions;
    return skipped;
}

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

    int run_n = 0;
    if (!fake6502_run_fast(g_cpu, g_run_stop, &run_n)) { fake6502_step(g_cpu); run_n = 1; }


    if (g_bus->oam_dma_pending) {
        emu_driver_sync_ppu();
        nes_bus_oam_dma_step(g_bus, g_cpu);
        g_apu_deadline = 0; DL_REFRESH();
        /* DMA kaynak sayfasi PPU/kartus alaninda olabilir ($2007 okumasi A12/mapper saati uretir): olay hesabini tazele. */
        g_ppu_deadline = 0; DL_REFRESH();
    }

    int ticks_after = g_cpu->emu.clockticks;
    int cpu_cycles = ticks_after - ticks_before;

    /* Tembel PPU: komut sonunda yalniz bir sonraki gorunur olayin deadline'ina
     * ulasildiysa senkronla. Deadline'a ulasilmadiysa PPU'nun disariya gorunen
     * hicbir durumu (NMI/IRQ kenari, mapper saati, yazmac) degisemez; bekleyen
     * dot'lar bir sonraki erisimde/deadline'da toplu calisir. */
    ppu_sync_if_due();

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
        /* NMI girisi bus cycle'lariyla ilerledi; deadline'a ulasildiysa burada senkronla. */
        ppu_sync_if_due();
    }


    /* geri (veya ayni adrese) dallanma = dongu adayi */
    if (g_ff_stop != 0 &&
        g_cpu->cpu.pc <= g_cpu->emu.instruction_pc) {
        cpu_cycles += idle_ff_candidate();
    }
    return cpu_cycles;
}