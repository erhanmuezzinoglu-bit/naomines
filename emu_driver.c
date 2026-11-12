#include "emu_driver.h"
#include "nes_bus.h"
#include "fake6502.h"
#include "apu.h"
#include "ppu.h"
#include "exp_config.h"
#include <string.h>
#include <stddef.h>

#if EXP_DIAG_PROF
/* Olcum kipi (bkz. exp_config.h EXP_DIAG_PROF). */
int g_prof_mode = 0;
static ppu_t g_shadow_ppu;
static apu_t g_shadow_apu;
static uint8_t g_sh_nmi_last;
static int g_sh_nmi_pending;
#endif
#if EXP_DIAG_HUD
/* CPU sayaclari (tani): hizli yol cagrisi, hizli yolda calisan komut, tek komutluk yavas adim. */
unsigned long g_st_rf_calls = 0, g_st_rf_instr = 0, g_st_slow = 0;
#endif

extern int mapper_irq_pending(void);
extern int mapper_ppu_timing_sensitive(void);

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
#if EXP_DIAG_PROF
    if ((g_prof_mode == 1 || g_prof_mode >= 3) && ppu == g_ppu) {
        extern int g_ppu_shadow_ablate;
        g_ppu_shadow_ablate = g_prof_mode >= 3 ? g_prof_mode - 2 : 0;   /* 3: sprite degerlendirme yok, 4: satir cizimi yok */
        extern int g_ppu_shadow_active;
        const int sk = ppu_get_render_skip();
        ppu_set_render_skip(1);
        g_ppu_shadow_active = 1;
        ppu_step_sampled(&g_shadow_ppu, cycles, &g_sh_nmi_last, &g_sh_nmi_pending);
        g_ppu_shadow_active = 0;
        ppu_set_render_skip(sk);
    }
#endif
}

#if EXP_DIAG_PROF
void emu_driver_apu_step(apu_t *a, int n)
{
    apu_step(a, n);
    if (g_prof_mode == 2 && a == &g_bus->apu) {
        extern int g_apu_shadow_active;
        g_apu_shadow_active = 1;
        apu_step(&g_shadow_apu, n);
        g_apu_shadow_active = 0;
        if (g_shadow_apu.sample_count > 4096u) g_shadow_apu.sample_count = 0;
    }
}

void emu_driver_prof_set_mode(int m)
{
    if ((m == 1 || m >= 3) && g_ppu) { memcpy(&g_shadow_ppu, g_ppu, sizeof g_shadow_ppu); ppu_oam_changed(); }
    if (m == 2 && g_bus) { memcpy(&g_shadow_apu, &g_bus->apu, sizeof g_shadow_apu); g_shadow_apu.sample_count = 0; }
    g_prof_mode = m;
}
#endif

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

/* TANI (davranisi degistirmez): $2002 (PPU durum) okuma sayisi ve diger yazmac/kartus erisimleri. */
unsigned long g_diag_status_reads = 0, g_diag_io = 0;
unsigned long g_diag_k[10];
#if EXP_DIAG_HUD
#define DIAGK(i) (g_diag_k[i]++)
#else
#define DIAGK(i) ((void)0)
#endif
/* $2002 yoklama atlamasi (EXP_POLL_FF): son $2002 okumasinin degeri ve aninin clockticks'i, okuma sayaci. */
static unsigned g_poll_cnt = 0;
static uint8_t  g_poll_val = 0;
static int      g_poll_tick = 0;
/* Yavas yolda (fake6502_mem_read) yapilan $2002 okumasi sayaci: step_1 bir sonraki hizli yol cagrisinin ilk geri dallanmada
 * donup donmeyecegini buna gore belirler (hizli yoldaki $2002 okumalari icin fake6502_fast.inc kendi kuralini uygular). */
static unsigned g_poll_slow = 0;
/* fake6502_fast.inc: onceki hizli yol cagrisi, hizli $2002 okumasi iceren ayni durumlu bir geri dallanmada dondu. */
int g_rf_poll_ret = 0;
unsigned long g_ffp_cycles_total = 0;   /* yoklama atlamasiyla gecilen cycle (tani) */
#if EXP_DIAG_HUD
/* Yoklama atlamasi tanisi (davranisi degistirmez): ret nedenleri sayaclari + son $2002 okumasini yapan komutun PC'si ve
 * son yoklama turunu kapatan geri dallanmanin PC'si. [0] durum farkli [1] baska yavas erisim / !=1 okuma [2] bit 7
 * [3] onceki tur farkli (K/r/v) [4] son tarih / kare siniri yakin [5] anlik PPU durumu farkli [6] kararli aralik kisa [7] atlandi */
unsigned long g_pdiag[8];
uint16_t g_pdiag_rpc, g_pdiag_bpc;
#define PDIAG(i) (g_pdiag[(i)]++)
#else
#define PDIAG(i) ((void)0)
#endif
/* fake6502_fast.inc: onceki adim yavas komut calistirdiysa 1 (bkz. hizli yolun geri dallanma suzgeci). */
int g_rf_after_slow = 0;
int g_rf_slow_done = 0;
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
    /* yoklama dongusu: onceki turun $2002 sayaci ve olculen tur (K), okuma ofseti (r), deger (v) */
    unsigned pcnt;
    int pk, pr;
    uint8_t pv;
    uint8_t phave;
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
    DIAGK(0);

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
        APU_STEP(&bus->apu, elapsed);
        bus->last_apu_sync_tick = (uint32_t)now;
    }
    DIAGK(1);

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
        APU_STEP(&g_bus->apu, elapsed);
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
    if ((uint16_t)(address - 0x2000u) < 0x4000u) { if ((address & 0xE007u) == 0x2002u) g_diag_status_reads++; else g_diag_io++; }
    if ((uint16_t)(address - 0x4000u) < 0x20u) DIAGK(5);
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
    /* Yalniz $4015 okumasi APU durumunu degistirir (kare IRQ bayragi silinir); $4000-$4014 acik bus, $4016/$4017 denetleyici. */
    if (address == 0x4015 || (address >= 0x4018 && address <= 0x401F)) {
        g_apu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }
    /* $2007 okumasi v'yi artirir (A12 yukselirse mapper saatlenir): IRQ'ya kalan saat sayisi degisebilir. Bu yalniz A12 sayacli
     * mapper'da (veya PPU konumunu okuyan SNROM PRG-RAM kuralinda) olaydir; bkz. mapper_ppu_timing_sensitive. */
    if (address >= 0x2000 && address <= 0x3FFF && (address & 7u) == 7u && mapper_ppu_timing_sensitive()) {
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

    if ((uint16_t)(address - 0x2000u) < 0x2000u && (address & 7u) == 2u) {
        g_poll_cnt++;
        g_poll_slow++;
        g_poll_val = result;
        g_poll_tick = c->emu.clockticks;
#if EXP_DIAG_HUD
        g_pdiag_rpc = c->emu.instruction_pc;
#endif
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
    if (address >= 0x2000u && (uint16_t)(address - 0x6000u) >= 0x2000u) g_diag_io++;
    if ((uint16_t)(address - 0x2000u) < 0x2000u) DIAGK(2); else if ((uint16_t)(address - 0x4000u) < 0x20u) DIAGK(3);
    else if (address >= 0x4020u && (uint16_t)(address - 0x6000u) >= 0x2000u) DIAGK(4);
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

    /* $4016 denetleyici strobe'u APU durumuna dokunmaz. */
    if (address >= 0x4000 && address <= 0x401F && address != 0x4016) {
        g_apu_deadline = c->emu.clockticks + 1; DL_REFRESH();
    }

    /* PPU yazmaci yazildi (ornegin $2000 ile NMI acildi): olasi yukselen kenar, eager
     * surucuyle ayni cycle'da yakalansin diye bir sonraki cycle'da senkronla. */
    /* + mapper yazmaclari ($4020-$5FFF, $8000+): MMC3 IRQ sayaci/latch/etkin degisir, siradaki IRQ'ya kalan A12 saati
     * (ppu_dots_to_next_event) degisir: esik, yazmadan ONCEKI duruma gore hesaplanmisti. PRG-RAM'e yazma mapper'a gitmez. */
    /* PPU olay tahmini (ppu_dots_to_next_event) yalniz PPUCTRL/PPUMASK'a ($2000/$2001; NMI etkinlestirme dahil) ve MMC3 A12
     * sayacina baglidir. $2002-$2007 ve mapper yazimlari ancak mapper_ppu_timing_sensitive() iken zorunlu senkron ister; aksi halde
     * eski deadline gecerli kalir (mutlak zaman, PPU durumu yazmadan once nes_bus_write icinde zaten senkronlandi). */
    if (((address >= 0x2000 && address <= 0x3FFF) && (address & 7u) <= 1u) ||
        (((address >= 0x2000 && address <= 0x3FFF) ||
          (address >= 0x4020 && (address < 0x6000 || address >= 0x8000))) && mapper_ppu_timing_sensitive())) {
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
    g_rf_poll_ret = 0;
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
    g_rf_poll_ret = 0;
    g_ff_lp.valid = 0;
    g_ff_lp.phave = 0;
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

/* ---- $2002 yoklama atlamasi (bkz. exp_config.h EXP_POLL_FF) ----
 * Tur [onceki geri-dallanma, simdi) icinde TEK yavas erisim bir $2002 okumasiysa (g_write_epoch ve g_poll_cnt birer arttiysa),
 * yazmaclar ayni ve son iki tur ayni uzunluk (K), ayni okuma ofseti (r) ve ayni okunan degerle (v) gectiyse: sonraki turlar
 * da ayni yolu izler ve okudugu deger PPU durumu degismedikce v'dir. v okunurken $2002'nin PPU'ya yan etkisi yoktur:
 * bit 7 zaten sifir (v'de bit 7 yok), w zaten 0, open_bus zaten v, bozunma sayaci $2002 ile sifirlanmaz; yaris penceresi
 * ppu_status_stable_dots tarafindan dislanir. Bus alanlari (open_bus_value vb.) turun son (hizli) erisimiyle her turda ayni
 * degere doner. Bu yuzden okumasi PPU'nun "durumun degismedigi kanitlanan" araligina dusen turlar, PPU adimlanmadan atlanir;
 * PPU tembel kalir ve bir sonraki gercek erisimde tek parcada ilerler. Ust sinirlar bos dongu atlamasiyla ayni (g_dl_min,
 * g_ff_stop): araliga CPU'ya gorunur bir olay (NMI/IRQ/DMC) dusmez. */
static inline int poll_ff_try(int K, int L, int r, uint8_t v)
{
    fake6502_context *const c = g_cpu;
    /* PPU okumadan sonra (son tarih senkronuyla) ilerlemis olabilir: SU ANKI durum v'yi vermeli. */
    if ((g_ppu->reg_status & 0xE0) != (v & 0xE0) || (g_ppu->open_bus & 0x1F) != (v & 0x1F)) { PDIAG(5); return 0; }
    const int now = c->emu.clockticks;
    int avail_dl   = (int)((uint32_t)g_dl_min - (uint32_t)now) - 1;
    int avail_stop = (int)((uint32_t)g_ff_stop - (uint32_t)now);
    int avail = avail_dl < avail_stop ? avail_dl : avail_stop;
    if (avail < K) { PDIAG(4); return 0; }

    const int lag = now - (int)g_bus->last_ppu_sync_tick;   /* PPU'nun geride kaldigi cycle (>= 0) */
    if (lag < 0) return 0;
    /* tur j'nin okumasi: now + j*K + r; PPU'nun o ana kadar ilerleyecegi dot: 3*(lag + j*K + r) */
    const uint32_t lim = 3u * (uint32_t)(lag + avail);
    const uint32_t ds = ppu_status_stable_dots(g_ppu, lim);
    const int first = lag + r;                                /* j = 0 icin CPU cycle mesafesi */
    if ((uint32_t)first * 3u > ds) { PDIAG(6); return 0; }
    int n1 = (int)(((ds / 3u) - (uint32_t)first) / (uint32_t)K) + 1;
    int n2 = avail / K;
    int N = n1 < n2 ? n1 : n2;
    if (N <= 0) { PDIAG(6); return 0; }
    PDIAG(7);

    c->emu.clockticks   += N * K;
    c->emu.instructions += N * L;
    g_ff_cycles_total  += (unsigned long)(N * K);
    g_ffp_cycles_total += (unsigned long)(N * K);
    g_ff_events_total++;
    return N * K;
}

static inline __attribute__((always_inline)) int idle_ff_candidate(void)
{
    fake6502_context *const c = g_cpu;
    int skipped = 0;
    uint32_t w0;
    memcpy(&w0, &c->cpu.a, sizeof w0);   /* a, x, y, flags: yapi basinda ardisik 4 bayt */

    const int same_state =
        g_ff_lp.valid &&
        g_ff_lp.pc == c->cpu.pc && g_ff_lp.s == c->cpu.s && g_ff_lp.w0 == w0 &&
        !g_nmi_pending && !g_nmi_ready && !c->emu.pending_irq &&
        !c->emu.nmi_defer_after_interrupt && !g_cpu_reset_in_progress &&
        g_bus->apu.dmc.dma_request == 0;

#if EXP_DIAG_HUD && EXP_POLL_FF
    if (g_poll_cnt != g_ff_lp.pcnt) {   /* son turda $2002 okundu: neden atlanamayacagini say */
        if (!same_state) PDIAG(0);
        else if (g_write_epoch - g_ff_lp.epoch != 1u || g_poll_cnt - g_ff_lp.pcnt != 1u) PDIAG(1);
        else if (g_poll_val & 0x80) PDIAG(2);
    }
#endif
    if (same_state && g_ff_lp.epoch == g_write_epoch) {

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
        g_ff_lp.phave = 0;
#if EXP_POLL_FF
    } else if (same_state &&
               g_write_epoch - g_ff_lp.epoch == 1u && g_poll_cnt - g_ff_lp.pcnt == 1u &&
               !(g_poll_val & 0x80)) {
        const int K = c->emu.clockticks - g_ff_lp.ticks;
        const int L = c->emu.instructions - g_ff_lp.instrs;
        const int r = g_poll_tick - g_ff_lp.ticks;
        if (K > 0 && L > 0 && r > 0 && r <= K) {
#if EXP_DIAG_HUD
            g_pdiag_bpc = c->emu.instruction_pc;
#endif
            if (g_ff_lp.phave && g_ff_lp.pk == K && g_ff_lp.pr == r && g_ff_lp.pv == g_poll_val) {
                skipped = poll_ff_try(K, L, r, g_poll_val);
            } else {
                PDIAG(3);
            }
            g_ff_lp.phave = 1; g_ff_lp.pk = K; g_ff_lp.pr = r; g_ff_lp.pv = g_poll_val;
        } else {
            g_ff_lp.phave = 0;
        }
        g_ff_lp.epoch = g_write_epoch;
#endif
    } else {
        g_ff_lp.valid = 1;
        g_ff_lp.w0    = w0;
        g_ff_lp.s     = c->cpu.s;
        g_ff_lp.pc    = c->cpu.pc;
        g_ff_lp.epoch = g_write_epoch;
        g_ff_lp.phave = 0;
    }

    g_ff_lp.pcnt   = g_poll_cnt;
    g_ff_lp.ticks  = c->emu.clockticks;
    g_ff_lp.instrs = c->emu.instructions;
    return skipped;
}

/* ---- Hizli yolda $2002 okumasi (fake6502_fast.inc RDV, yalniz salt okuma islemlerinin son okumasi) ----
 * Yavas yolun yaptiginin aynisi: fake6502_mem_read -> nes_bus_read: PPU okuma anina (tick) senkronlanir (sync_ppu ile ayni
 * adimlama), ppu_cpu_read($2002) degeri ve yan etkileri (bit 7, w, open bus, yaris bastirmasi) uygulanir. Hizli yol cagrisi boyunca
 * tick < g_dl_min - 8 oldugundan bu senkron hicbir olay (NMI/IRQ kenari) gecmez ve deadline'i degistirmez; DMC istegi yoktur, APU
 * senkronu gerekmez ($2002). CPU tarafi bus alanlari (open bus, son okuma adresi, $4016 dizisi) hizli yolun cikisinda yazilir.
 * Surucu sayaclari (yazma epoch'u, $2002 sayaci/degeri/ani, tani) yavas yolla ayni sekilde guncellenir. */
int emu_driver_fast_status_read(int tick, uint16_t ipc, uint8_t *out)
{
    nes_bus_t *const bus = g_bus;
    const int elapsed = tick - (int)bus->last_ppu_sync_tick;
    if (elapsed > 0) {
        step_ppu_with_nmi_probe(bus, (uint32_t)(elapsed * 3));
        bus->last_ppu_sync_tick = (uint32_t)tick;
    }
    const uint8_t v = ppu_cpu_read(bus->ppu, 0x2002);
    g_write_epoch++;
    g_diag_status_reads++;
    g_poll_cnt++;
    g_poll_val = v;
    g_poll_tick = tick;
#if EXP_DIAG_HUD
    g_pdiag_rpc = ipc;
    g_diag_k[8]++;
#else
    (void)ipc;
#endif
    *out = v;
    return 1;
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
    const unsigned poll0 = g_poll_slow;
    g_rf_slow_done = 0;
    if (!fake6502_run_fast(g_cpu, g_run_stop, &run_n)) { fake6502_step(g_cpu); run_n = 0; g_rf_slow_done = 1; }
    /* Yavas komut (tek basina ya da hizli komutlarin ardindan) bir $2002 okumasi yaptiysa, bir sonraki hizli yol cagrisi
     * ilk geri dallanmada doner: yoklama atlamasi her turun sonunda gozlem ister. Diger yavas komutlardan sonra gerekmez
     * (bos dongu atlamasi zaten yazma/yavas erisim olmayan turlar ister; hizli yolun geri dallanma suzgeci onlari bildirir). */
    g_rf_after_slow = (g_poll_slow != poll0);
#if EXP_DIAG_HUD
    g_st_slow += (unsigned long)g_rf_slow_done;
    g_st_rf_instr += (unsigned long)run_n;
#endif
#if EXP_DIAG_HUD
    g_st_rf_calls++;
#endif


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