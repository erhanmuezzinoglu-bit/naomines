#include "mapper.h"
#include "nes_rom.h"
#include <string.h>
#include <stdbool.h>
#include "exp_config.h"
#include "nes_bus.h"
#if EXP_DIAG_HUD
extern unsigned long g_diag_k[10];
#define MDIAGK(i) (g_diag_k[i]++)
#else
#define MDIAGK(i) ((void)0)
#endif

/* MMC3 IRQ state. */
static int mmc3_irq_pending = 0;
/* MMC3 bank esleme degisiklik izleme (bkz. mmc3_update). */
static uint8_t mmc3_ap_regs[8];
static uint8_t mmc3_ap_mode;
static uint8_t mmc3_ap_valid;

/* MMC2 bus pointer. mapper_map_prg_pages() bunu bus init'te doldurur. */
static nes_bus_t *g_mapper_bus = NULL;

/* emu_driver ile döngüsel include'a girmemek için forward decl. */
extern void emu_driver_request_irq(void);
extern void emu_driver_clear_irq(void);

/* ============================================================
 *  Simple mapper runtime state (file-local).
 *
 *  These mappers (11, 34, 66, 78, 180, 28) do not need entries
 *  in mapper_state_t because they store at most a few bytes.
 *  Keeping them file-local avoids touching mapper.h / nes_bus.h.
 * ============================================================ */
static uint8_t m11_prg = 0;
static uint8_t m11_chr = 0;

static uint8_t m34_prg = 0;

static uint8_t m66_prg = 0;
static uint8_t m66_chr = 0;

static uint8_t m78_prg = 0;
static uint8_t m78_chr = 0;
static uint8_t m78_mirror = 0;

/* A53 (Action 53 / INL-ROM, Mapper 28) runtime state. */
static uint8_t a53_select = 0;   /* son $5000 yazımı (register index + write-enable) */
static uint8_t a53_mode = 0;     /* register $80 */
static uint8_t a53_outer = 0;    /* register $81 */
static uint8_t a53_prg = 0;      /* register $01 */
static uint8_t a53_chr = 0;      /* register $00 */

/* ============================================================
 *  Mapper state reset
 * ============================================================ */
static void mapper_tags_clear(void);

void mapper_reset_state(void)
{
    memset(&g_mapper, 0, sizeof(g_mapper));
    g_prgram_ppu_dep = 0;
    mapper_tags_clear();
    mmc3_ap_valid = 0;   /* bir sonraki MMC3 guncellemesi tam olsun */

    /* MMC1 power-on/reset defaults. */
    g_mapper.mmc1_shift = 0x10;
    g_mapper.mmc1_control = 0x0C;

    /* MMC2 latch'leri reset'te FE durumundadir. */
    g_mapper.mmc2_latch0 = 0xFE;
    g_mapper.mmc2_latch1 = 0xFE;

    /* Runtime IRQ state mapper reset ile mutlaka sifirlanir. */
    mmc3_irq_pending = 0;

    /* MMC3 PRG-RAM: enabled + write-enabled by default for compatibility. */
    g_mapper.mmc3_prg_ram = 0x80;

    /* Simple mappers (11, 34, 66, 78). */
    m11_prg = 0;
    m11_chr = 0;
    m34_prg = 0;
    m66_prg = 0;
    m66_chr = 0;
    m78_prg = 0;
    m78_chr = 0;
    m78_mirror = 0;

    /* A53 (Mapper 28). */
    a53_select = 0;
    a53_mode = 0;
    /* Acilista dis bank = son 32K ($FF): reset vektoru son bankta (NESdev Action 53: acilis durumu son bank olmali;
     * eskiden 0'di -> Streemerz ilk 32K'dan acilip coküyordu). */
    a53_outer = 0xFF;
    a53_prg = 0;
    a53_chr = 0;
}

/* ============================================================
 *  MMC3 IRQ (scanline counter)
 * ============================================================ */
static void mmc3_clock_irq(void)
{
    if (g_mapper.mmc3_irq_counter == 0 || g_mapper.mmc3_irq_reload) {
        g_mapper.mmc3_irq_counter = g_mapper.mmc3_irq_latch;
        g_mapper.mmc3_irq_reload = 0;
    } else {
        g_mapper.mmc3_irq_counter--;
    }

    if (g_mapper.mmc3_irq_counter == 0 && g_mapper.mmc3_irq_enable) {
        mmc3_irq_pending = 1;
        emu_driver_request_irq();
    }
}

void mapper_scanline_clock(void)
{
    if (nes_last_mapper_loaded == 4) {
        mmc3_clock_irq();
    }
}

/* Siradaki A12 saatlerinden kacincisinde IRQ sinyali (CPU'ya gorunen) YUKSELEBILIR? 1 = siradaki saat; 0 = hicbir zaman
 * (mapper 4 degil veya IRQ kapali: saatler yalniz sayaci degistirir, CPU gormez; $C000/$C001/$E000/$E001 yazimlari
 * senkrondan sonra islenir). mmc3_clock_irq ile ayni mantik: sayac 0 veya reload ise saatte latch'e yuklenir,
 * degilse azalir; sayac 0 olunca IRQ. */
int mapper_scanline_clocks_until_irq(void)
{
    if (nes_last_mapper_loaded != 4) return 0;
    if (!g_mapper.mmc3_irq_enable) return 0;

    if (g_mapper.mmc3_irq_counter == 0 || g_mapper.mmc3_irq_reload) {
        return (g_mapper.mmc3_irq_latch == 0) ? 1 : 1 + (int)g_mapper.mmc3_irq_latch;
    }
    return (int)g_mapper.mmc3_irq_counter;
}

/* PPU'nun hangi ana senkronlandigi CPU'ya gorunur bir sonucu degistirebilir mi? (emu_driver.c, yazmac erisiminden sonraki zorunlu
 * PPU senkronu icin.) Iki durum var:
 *  - MMC3: PPU olay tahmini (ppu_dots_to_next_event) mapper_scanline_clocks_until_irq()'ya bagli; yazmac/mapper erisimi bunu degistirebilir.
 *  - MMC1 SNROM: PRG-RAM /CE, PPU'nun o anki konumundan (A12 tarafi) hesaplanir ve PPU senkronlanmadan okunur; R1/R2 bit 4 farkliysa
 *    sonuc PPU'nun son senkron anina baglidir.
 * Diger mapper'larda PPU olay tahmini yalniz PPUCTRL/PPUMASK'a baglidir ve PPU konumunu okuyan baska bir CPU yolu yoktur. */
int mapper_ppu_position_sensitive(void)
{
    return nes_last_mapper_loaded == 1 && !nes_chr_is_rom && nes_chr_banks_count == 8 &&
           ((g_mapper.mmc1_chr0 ^ g_mapper.mmc1_chr1) & 0x10u);
}

/* PRG-RAM'in PPU konumuna bagli oldugu durum (g_prgram_ppu_dep, = mapper_ppu_position_sensitive): bu durumda PRG-RAM erisimleri
 * erisim aninda PPU'yu senkronlayan yavas yoldan yapilir (nes_bus.c nes_bus_read/write); boylece PPU'nun "son senkron konumu"
 * hicbir CPU sonucunu etkilemez ve yazmac/mapper erisimlerinden sonra zorunlu senkron gerekmez. Bayrak yalniz mapper yazimi,
 * yeniden esleme, MMC2 kurulumu ve yuklemede degisebilir (genel giris noktalarinin sonunda yenilenir). */
int g_prgram_ppu_dep = 0;
void mapper_refresh_ppu_dep(void) { g_prgram_ppu_dep = mapper_ppu_position_sensitive(); }

int mapper_ppu_timing_sensitive(void)
{
    /* Yalniz MMC3: PPU olay tahmini A12 sayacina bagli. SNROM PRG-RAM kurali artik erisim aninda senkronlu (yukari). */
    return nes_last_mapper_loaded == 4;
}

int mapper_irq_pending(void)
{
    if (nes_last_mapper_loaded == 4) {
        return mmc3_irq_pending;
    }
    return 0;
}

/* ============================================================
 *  Ortak yardımcılar
 * ============================================================ */
/* Pencere etiketleri: her PRG 8 KB penceresinde ($8000,$A000,$C000,$E000) ve her CHR 1 KB yuvasinda (0..7) su anda hangi
 * KAYNAK bankin kopyalanmis oldugunu tutar; ayni kaynak yeniden istenirse kopyalama atlanir (icerik zaten ayni).
 * Bu pencereleri YALNIZ asagidaki yardimcilar degistirir. Etiketler mapper_reset_state() ve mapper_map_prg_pages()
 * basinda silinir (ROM yukleme, ppu_init). CHR yalniz CHR-ROM'da etiketlenir (CHR-RAM'e PPU yazabilir). */
static const uint8_t *g_prg_tag[4];
static const uint8_t *g_chr_tag[8];

static void mapper_tags_clear(void)
{
    for (int i = 0; i < 4; i++) g_prg_tag[i] = 0;
    for (int i = 0; i < 8; i++) g_chr_tag[i] = 0;
}

static void set_prg_8k(nes_bus_t *bus, uint32_t addr, uint8_t *src)
{
    if (!src) return;
    if (addr >= 0x8000u && addr <= 0xE000u && (addr & 0x1FFFu) == 0) {
        const uint32_t w = (addr - 0x8000u) >> 13;
        if (g_prg_tag[w] == src) return;
        g_prg_tag[w] = src;
#if EXP_PRG_PTR
        MDIAGK(6);
        nes_prg_page[w] = src;   /* kopya yok: pencere kaynagi; goruntu genel giris noktasinin sonunda eslenir (PRG_COMMIT) */
        nes_prg_dirty = 1;
        return;
#endif
    }
    MDIAGK(6);
    memcpy(&bus->mem[addr], src, 8192);
}

static void set_prg_16k(nes_bus_t *bus, uint32_t addr, int bank16k)
{
    int b8 = bank16k * 2;
    if (b8 < 0 || b8 + 1 >= nes_prg_banks_count) return;

    set_prg_8k(bus, addr,        nes_prg_banks[b8]);
    set_prg_8k(bus, addr + 8192, nes_prg_banks[b8 + 1]);
}

static void set_chr_1k(nes_bus_t *bus, int slot, int bank_1k)
{
    if (bank_1k < 0 || bank_1k >= nes_chr_banks_count) return;
    if (!bus->ppu || !nes_chr_banks[bank_1k]) return;
    if (slot >= 0 && slot < 8) {
        if (nes_chr_is_rom) {
            if (g_chr_tag[slot] == nes_chr_banks[bank_1k]) return;
            g_chr_tag[slot] = nes_chr_banks[bank_1k];
        } else {
            g_chr_tag[slot] = 0;
        }
    }
    MDIAGK(7);
    memcpy(&bus->ppu->vram[slot * 1024], nes_chr_banks[bank_1k], 1024);
}

static void set_chr_4k(nes_bus_t *bus, int slot4k, int bank_4k)
{
    for (int i = 0; i < 4; i++) {
        set_chr_1k(bus, slot4k * 4 + i, bank_4k * 4 + i);
    }
}

/* ============================================================
 *  32K PRG bankı $8000-$FFFF'e yerleştirir (M11/M34/M66).
 * ============================================================ */
static void set_prg_32k(nes_bus_t *bus, int bank32k)
{
    int total = nes_prg_banks_count / 4;
    if (total <= 0) return;
    bank32k = ((bank32k % total) + total) % total;
    int b8 = bank32k * 4;
    if (b8 + 3 >= nes_prg_banks_count) return;
    set_prg_8k(bus, 0x8000, nes_prg_banks[b8]);
    set_prg_8k(bus, 0xA000, nes_prg_banks[b8 + 1]);
    set_prg_8k(bus, 0xC000, nes_prg_banks[b8 + 2]);
    set_prg_8k(bus, 0xE000, nes_prg_banks[b8 + 3]);
}

/* ============================================================
 *  16K PRG bankını $8000-$BFFF'e yerleştirir (M78).
 * ============================================================ */
static void set_prg_16k_switch_low(nes_bus_t *bus, int bank16k)
{
    int total = nes_prg_banks_count / 2;
    if (total <= 0) return;
    bank16k = ((bank16k % total) + total) % total;
    int b8 = bank16k * 2;
    if (b8 + 1 >= nes_prg_banks_count) return;
    set_prg_8k(bus, 0x8000, nes_prg_banks[b8]);
    set_prg_8k(bus, 0xA000, nes_prg_banks[b8 + 1]);
}

/* ============================================================
 *  Son 16K PRG bankını $C000-$FFFF'e yerleştirir (M78).
 * ============================================================ */
static void set_prg_last_16k(nes_bus_t *bus)
{
    int total = nes_prg_banks_count;
    if (total < 2) return;
    set_prg_8k(bus, 0xC000, nes_prg_banks[total - 2]);
    set_prg_8k(bus, 0xE000, nes_prg_banks[total - 1]);
}

/* ============================================================
 *  8K CHR bankını PPU $0000-$1FFF'e yerleştirir (M11/M66/M78/M28).
 *  Yalnız CHR-ROM varken anlamlıdır; CHR-RAM'de erken çıkış yapar.
 * ============================================================ */
static void set_chr_8k_bank(nes_bus_t *bus, int bank8k)
{
    if (!bus->ppu || !nes_chr_is_rom) return;
    int total = nes_chr_banks_count / 8;
    if (total <= 0) return;
    bank8k = ((bank8k % total) + total) % total;
    int b1 = bank8k * 8;
    for (int i = 0; i < 8; i++) {
        set_chr_1k(bus, i, b1 + i);
    }
}

/* ============================================================
 *  A53 (Action 53 / INL-ROM, Mapper 28)
 *
 *  Çoklu-discrete mapper. PRG bank seçimi tek yazımla değil,
 *  register $80/$81/$01 kombinasyonuyla hesaplanır. Kaynak kod:
 *  Holy Mapperel src/drivers.s içindeki a53_calc_prg_bank().
 * ============================================================ */
static uint8_t a53_calc_prg_bank(
    uint8_t v80,
    uint8_t v01,
    uint8_t v81,
    int cpu_a14)
{
    /* outer_bank: $81 << 1 | A14 */
    uint8_t outer_bank =
        (uint8_t)(((v81 << 1) | (cpu_a14 ? 1 : 0)) & 0xFF);

    /* bank_mode: mode'un üst bitleri (bit 2-7) */
    uint8_t bank_mode = (uint8_t)(v80 >> 2);

    /* current_bank: $01 değeri */
    uint8_t current_bank = v01;

    /* UNROM modu aktifse ve A14 mode bit 0 ile eşleşiyorsa 32K moda düş */
    if ((bank_mode & 0x02) != 0) {
        if (((outer_bank ^ bank_mode) & 0x01) == 0) {
            bank_mode = 0;
        }
    }

    /* 32K switched modda A14'ü current_bank'e kaydır */
    if ((bank_mode & 0x02) == 0) {
        current_bank = (uint8_t)(
            (current_bank << 1) | (outer_bank & 0x01)
        );
    }

    /* Maske: mode bit 4-5 → 1, 3, 7, 15 */
    static const uint8_t masks[4] = { 1, 3, 7, 15 };
    uint8_t mask = masks[(bank_mode >> 2) & 0x03];

    return (uint8_t)(
        ((current_bank ^ outer_bank) & mask) ^ outer_bank
    );
}

static void a53_apply(nes_bus_t *bus)
{
    if (!bus) return;

    int total_16k = nes_prg_banks_count / 2;
    if (total_16k <= 0) return;

    uint8_t b0 = a53_calc_prg_bank(
        a53_mode, a53_prg, a53_outer, 0
    );
    uint8_t b1 = a53_calc_prg_bank(
        a53_mode, a53_prg, a53_outer, 1
    );

    /* 2MB üzerindeki 16K bank → gerçek ROM boyutuna indir */
    b0 = (uint8_t)(b0 % total_16k);
    b1 = (uint8_t)(b1 % total_16k);

    set_prg_16k(bus, 0x8000, b0);
    set_prg_16k(bus, 0xC000, b1);

    if (bus->ppu) {
        /* MMC1 tarzı mirroring: 0=1scr low, 1=1scr up, 2=V, 3=H */
        switch (a53_mode & 0x03) {
            case 0: bus->ppu->mirroring = 2; break;
            case 1: bus->ppu->mirroring = 3; break;
            case 2: bus->ppu->mirroring = 1; break;
            case 3: bus->ppu->mirroring = 0; break;
        }
    }

    /* CHR-ROM bank (CHR-RAM'de erken çıkış yapar) */
    set_chr_8k_bank(bus, a53_chr);
}

/* ============================================================
 *  UNROM (Mapper 2)
 * ============================================================ */
static void unrom_update(nes_bus_t *bus)
{
    int total_16k = nes_prg_banks_count / 2;
    if (total_16k <= 0) return;

    int bank = g_mapper.unrom_bank % total_16k;

    set_prg_16k(bus, 0x8000, bank);
    set_prg_16k(bus, 0xC000, total_16k - 1);

}

/* ============================================================
 *  CNROM (Mapper 3)
 * ============================================================ */
static void cnrom_update(nes_bus_t *bus)
{
    if (!bus->ppu || !nes_chr_is_rom) return;

    int total_8k = nes_chr_banks_count / 8;
    if (total_8k <= 0) return;

    int bank = g_mapper.cnrom_bank % total_8k;

    for (int i = 0; i < 8; i++) {
        set_chr_1k(bus, i, bank * 8 + i);
    }
}

/* ============================================================
 *  MMC1 (Mapper 1)
 * ============================================================ */
static bool mmc1_consecutive_write(nes_bus_t *bus)
{
    if (!bus || !g_mapper.mmc1_last_write_valid) {
        return false;
    }

    /* nes_bus_write() PPU/APU sync'inden sonra bu deger mevcut CPU tick'idir. */
    uint32_t tick = (uint32_t)bus->last_ppu_sync_tick;
    return tick == g_mapper.mmc1_last_write_tick + 1u;
}

static void mmc1_remember_write(nes_bus_t *bus)
{
    if (!bus) return;
    g_mapper.mmc1_last_write_tick = (uint32_t)bus->last_ppu_sync_tick;
    g_mapper.mmc1_last_write_valid = 1;
}

static void mmc1_update_prg(nes_bus_t *bus)
{
    int total_16k = nes_prg_banks_count / 2;
    if (total_16k <= 0) return;

    int bank = (g_mapper.mmc1_prg & 0x0Fu) % total_16k;
    uint8_t prg_mode = (g_mapper.mmc1_control >> 2) & 0x03;

    if (prg_mode <= 1) {
        int b = (bank & ~1) % total_16k;
        set_prg_16k(bus, 0x8000, b);
        set_prg_16k(bus, 0xC000, (b + 1) % total_16k);
    } else if (prg_mode == 2) {
        set_prg_16k(bus, 0x8000, 0);
        set_prg_16k(bus, 0xC000, bank);
    } else {
        set_prg_16k(bus, 0x8000, bank);
        set_prg_16k(bus, 0xC000, total_16k - 1);
    }
}

static void mmc1_update_chr(nes_bus_t *bus)
{
    if (!bus->ppu || !nes_chr_is_rom) return;

    int total_4k = nes_chr_banks_count / 4;
    if (total_4k <= 0) return;

    if (g_mapper.mmc1_control & 0x10) {
        int b0 = g_mapper.mmc1_chr0 % total_4k;
        int b1 = g_mapper.mmc1_chr1 % total_4k;
        set_chr_4k(bus, 0, b0);
        set_chr_4k(bus, 1, b1);
    } else {
        int b = (g_mapper.mmc1_chr0 & ~1) % total_4k;
        set_chr_4k(bus, 0, b);
        set_chr_4k(bus, 1, (b + 1) % total_4k);
    }
}

static void mmc1_update_mirroring(nes_bus_t *bus)
{
    if (!bus->ppu) return;

    uint8_t m = g_mapper.mmc1_control & 0x03;
    switch (m) {
        case 0: bus->ppu->mirroring = 2; break;
        case 1: bus->ppu->mirroring = 3; break;
        case 2: bus->ppu->mirroring = 1; break;
        case 3: bus->ppu->mirroring = 0; break;
    }
}

static void mmc1_update(nes_bus_t *bus)
{
    mmc1_update_prg(bus);
    mmc1_update_chr(bus);
    mmc1_update_mirroring(bus);
}

/*
 * SNROM WRAM /CE modeli.
 *
 * SNROM, 8KB CHR-RAM kullanan MMC1 kartlarinda CHR A16 hattini
 * PRG-RAM /CE icin de kullanir. MMC1_A12 testinde mapper 4KB CHR
 * modundadir ve R1 bit4=0, R2 bit4=1 olarak ayarlanir. PPU A12=0
 * iken R1; PPU A12=1 iken R2 CHR banki secildiginden, bit4 WRAM'yi
 * acip kapatir.
 *
 * PPU dosyasina dokunmadan, mevcut PPU cycle/model bilgisinden o anki
 * pattern-table tarafini cikartiyoruz. Bu yalnizca SNROM kosulunda
 * etkinlesir; CHR-ROM kullanan MMC1 kartlari mevcut davranisini korur.
 */
static bool mmc1_snrom_prg_ram_enabled(const nes_bus_t *bus)
{
    if (!bus || !bus->ppu) return true;

    /* SNROM: 8KB CHR-RAM. */
    if (nes_chr_is_rom || nes_chr_banks_count != 8) {
        return true;
    }

    const ppu_t *ppu = bus->ppu;
    bool ppu_a12 = false;

    if ((ppu->reg_mask & 0x18u) &&
        (ppu->scanline < 240 || ppu->scanline == 261)) {
        if (ppu->cycle >= 257 && ppu->cycle <= 320) {
            /* Sprite pattern fetchleri: PPU A12 = PPUCTRL bit 3. */
            ppu_a12 = (ppu->reg_ctrl & 0x08u) != 0;
        } else if (ppu->cycle >= 1 && ppu->cycle <= 256) {
            /* Background pattern fetchleri: PPU A12 = PPUCTRL bit 4. */
            ppu_a12 = (ppu->reg_ctrl & 0x10u) != 0;
        }
    }

    uint8_t chr_bank =
        ppu_a12 ? g_mapper.mmc1_chr1 : g_mapper.mmc1_chr0;

    return (chr_bank & 0x10u) == 0;
}

/*
 * MMC1B PRG register bit 4 disables PRG-RAM. Keep the SNROM CHR-bank
 * /CE rule as an additional condition for boards that use it.
 */
static bool mmc1_prg_ram_is_enabled(const nes_bus_t *bus)
{
    if (g_mapper.mmc1_prg & 0x10u) {
        return false;
    }

    return mmc1_snrom_prg_ram_enabled(bus);
}

/*
 * CPU $6000-$7FFF PRG-RAM'in mapper tarafindan etkin olup olmadigini
 * bildirir. MMC3 kontrolu da burada merkezilestirilmistir.
 */
int mapper_prg_ram_enabled(const nes_bus_t *bus)
{
    if (nes_last_mapper_loaded == 1) {
        return mmc1_prg_ram_is_enabled(bus) ? 1 : 0;
    }

    if (nes_last_mapper_loaded == 4) {
        return (g_mapper.mmc3_prg_ram & 0x80u) ? 1 : 0;
    }

    return 1;
}

int mapper_prg_ram_write_enabled(const nes_bus_t *bus)
{
    if (nes_last_mapper_loaded == 1) {
        return mmc1_prg_ram_is_enabled(bus) ? 1 : 0;
    }

    if (nes_last_mapper_loaded == 4) {
        return ((g_mapper.mmc3_prg_ram & 0x80u) != 0) &&
               ((g_mapper.mmc3_prg_ram & 0x40u) == 0);
    }

    return 1;
}

static void mmc1_write(uint16_t addr, uint8_t value, nes_bus_t *bus)
{
    /* MMC1 ignores consecutive PRG-RAM/ROM write cycles after the first. */
    if (mmc1_consecutive_write(bus)) {
        return;
    }
    mmc1_remember_write(bus);

    if (value & 0x80) {
        g_mapper.mmc1_shift = 0x10;
        g_mapper.mmc1_control |= 0x0C;
        mmc1_update(bus);
        return;
    }

    bool complete = (g_mapper.mmc1_shift & 0x01) != 0;
    g_mapper.mmc1_shift >>= 1;
    g_mapper.mmc1_shift |= (uint8_t)((value & 0x01) << 4);

    if (!complete) return;

    uint8_t data = g_mapper.mmc1_shift & 0x1F;

    if (addr <= 0x9FFF) {
        g_mapper.mmc1_control = data;
    } else if (addr <= 0xBFFF) {
        g_mapper.mmc1_chr0 = data;
    } else if (addr <= 0xDFFF) {
        g_mapper.mmc1_chr1 = data;
    } else {
        g_mapper.mmc1_prg = data & 0x1F;
    }

    g_mapper.mmc1_shift = 0x10;
    mmc1_update(bus);
}

/* ============================================================
 *  MMC3 (Mapper 4)
 * ============================================================ */
static void mmc3_update_prg(nes_bus_t *bus)
{
    int total_8k = nes_prg_banks_count;
    if (total_8k <= 0) return;

    int r6 = g_mapper.mmc3_regs[6] % total_8k;
    int r7 = g_mapper.mmc3_regs[7] % total_8k;

    int b8000 = r6;
    int bA000 = r7;
    int bC000 = (total_8k >= 2) ? total_8k - 2 : 0;
    int bE000 = (total_8k >= 1) ? total_8k - 1 : 0;

    if (g_mapper.mmc3_select & 0x40) {
        int tmp = b8000;
        b8000 = bC000;
        bC000 = tmp;
    }

    set_prg_8k(bus, 0x8000, nes_prg_banks[b8000]);
    set_prg_8k(bus, 0xA000, nes_prg_banks[bA000]);
    set_prg_8k(bus, 0xC000, nes_prg_banks[bC000]);
    set_prg_8k(bus, 0xE000, nes_prg_banks[bE000]);
}

static void mmc3_update_chr(nes_bus_t *bus)
{
    if (!bus->ppu || !nes_chr_is_rom) return;

    int total_1k = nes_chr_banks_count;
    if (total_1k <= 0) return;

    uint8_t *r = g_mapper.mmc3_regs;

    if (!(g_mapper.mmc3_select & 0x80)) {
        int b0 = (r[0] & 0xFE) % total_1k;
        int b1 = (r[1] & 0xFE) % total_1k;
        set_chr_1k(bus, 0, b0);
        set_chr_1k(bus, 1, (b0 + 1) % total_1k);
        set_chr_1k(bus, 2, b1);
        set_chr_1k(bus, 3, (b1 + 1) % total_1k);
        set_chr_1k(bus, 4, r[2] % total_1k);
        set_chr_1k(bus, 5, r[3] % total_1k);
        set_chr_1k(bus, 6, r[4] % total_1k);
        set_chr_1k(bus, 7, r[5] % total_1k);
    } else {
        set_chr_1k(bus, 0, r[2] % total_1k);
        set_chr_1k(bus, 1, r[3] % total_1k);
        set_chr_1k(bus, 2, r[4] % total_1k);
        set_chr_1k(bus, 3, r[5] % total_1k);
        int b0 = (r[0] & 0xFE) % total_1k;
        int b1 = (r[1] & 0xFE) % total_1k;
        set_chr_1k(bus, 4, b0);
        set_chr_1k(bus, 5, (b0 + 1) % total_1k);
        set_chr_1k(bus, 6, b1);
        set_chr_1k(bus, 7, (b1 + 1) % total_1k);
    }
}

static void mmc3_update_mirroring(nes_bus_t *bus)
{
    if (!bus->ppu) return;

    /* MMC3 A000 bit0: 0=vertical, 1=horizontal. */
    bus->ppu->mirroring = g_mapper.mmc3_mirroring ? 0 : 1;
}

/* Degisiklik izleme: son UYGULANAN bank yazmaclari ve mod bitleri. Eskiden her mapper yazmasi ($8000/$8001 dahil, hatta
 * IRQ/aynalama yazmaclari) 4x8 KB PRG + 8x1 KB CHR'yi bastan kopyaliyordu (~40 KB). Esleme yalnizca (regs[], select & 0xC0,
 * toplam bank sayilari) ile belirlendigi icin, degisen pencereler yeniden kopyalanir; sonuc birebir aynidir.
 * mmc3_ap_valid = 0 (ROM yukleme / mapper_reset_state) -> bir sonraki guncelleme tamdir. */
static int mmc3_chr_bank(int slot)
{
    const int total_1k = nes_chr_banks_count;
    const uint8_t *r = g_mapper.mmc3_regs;
    int s = slot;
    if (g_mapper.mmc3_select & 0x80) s = (slot + 4) & 7;   /* CHR ters cevrim: pencere cifti degisir */
    if (s < 4) {
        const int b = (r[s >> 1] & 0xFE) % total_1k;       /* R0 (slot 0,1), R1 (slot 2,3): 2 KB */
        return (s & 1) ? (b + 1) % total_1k : b;
    }
    return r[s - 2] % total_1k;                            /* R2..R5 (slot 4..7) */
}

static void mmc3_apply_chr_slot(nes_bus_t *bus, int slot)
{
    set_chr_1k(bus, slot, mmc3_chr_bank(slot));
}

static void mmc3_apply_prg_window(nes_bus_t *bus, int win)   /* 0: $8000, 1: $A000, 2: $C000, 3: $E000 */
{
    const int total_8k = nes_prg_banks_count;
    const int r6 = g_mapper.mmc3_regs[6] % total_8k;
    const int r7 = g_mapper.mmc3_regs[7] % total_8k;
    int b[4] = { r6, r7, (total_8k >= 2) ? total_8k - 2 : 0, (total_8k >= 1) ? total_8k - 1 : 0 };
    if (g_mapper.mmc3_select & 0x40) { const int t = b[0]; b[0] = b[2]; b[2] = t; }
    set_prg_8k(bus, 0x8000 + win * 0x2000, nes_prg_banks[b[win]]);
}

static void mmc3_update(nes_bus_t *bus)
{
    const uint8_t mode = (uint8_t)(g_mapper.mmc3_select & 0xC0);

    if (!mmc3_ap_valid || mode != mmc3_ap_mode) {
        mmc3_update_prg(bus);
        mmc3_update_chr(bus);
    } else {
        const uint8_t *r = g_mapper.mmc3_regs;
        const int chr_ok = bus->ppu && nes_chr_is_rom && nes_chr_banks_count > 0;
        const int prg_ok = nes_prg_banks_count > 0;
        const int inv = (mode & 0x80) != 0;
        const int swp = (mode & 0x40) != 0;
        for (int i = 0; i < 8; i++) {
            if (r[i] == mmc3_ap_regs[i]) continue;
            if (i < 2) {
                if (chr_ok) { const int s0 = (inv ? 4 : 0) + i * 2; mmc3_apply_chr_slot(bus, s0); mmc3_apply_chr_slot(bus, s0 + 1); }
            } else if (i < 6) {
                if (chr_ok) mmc3_apply_chr_slot(bus, (inv ? 0 : 4) + (i - 2));
            } else if (prg_ok) {
                mmc3_apply_prg_window(bus, i == 6 ? (swp ? 2 : 0) : 1);
            }
        }
    }

    memcpy(mmc3_ap_regs, g_mapper.mmc3_regs, 8);
    mmc3_ap_mode = mode;
    mmc3_ap_valid = 1;
    mmc3_update_mirroring(bus);
}

static void mmc3_write(uint16_t addr, uint8_t value, nes_bus_t *bus)
{
    if (addr < 0x8000) return;

    if ((addr & 0xE001) == 0x8000) {
        g_mapper.mmc3_select = value;
    }
    else if ((addr & 0xE001) == 0x8001) {
        uint8_t reg = g_mapper.mmc3_select & 0x07;
        g_mapper.mmc3_regs[reg] = value;
    }
    else if ((addr & 0xE001) == 0xA000) {
        g_mapper.mmc3_mirroring = value & 0x01;
    }
    else if ((addr & 0xE001) == 0xA001) {
        /* MMC3 PRG-RAM control: bit 7 enable, bit 6 write protect. */
        g_mapper.mmc3_prg_ram = value;
    }
    else if ((addr & 0xE001) == 0xC000) {
        g_mapper.mmc3_irq_latch = value;
    }
    else if ((addr & 0xE001) == 0xC001) {
        /* Clear now; reload from C000 on the NEXT A12 rising edge. */
        g_mapper.mmc3_irq_counter = 0;
        g_mapper.mmc3_irq_reload = 1;
    }
    else if ((addr & 0xE001) == 0xE000) {
        g_mapper.mmc3_irq_enable = 0;
        if (mmc3_irq_pending) {
            mmc3_irq_pending = 0;
            emu_driver_clear_irq();
        }
    }
    else if ((addr & 0xE001) == 0xE001) {
        g_mapper.mmc3_irq_enable = 1;
    }

    mmc3_update(bus);
}

/* ============================================================
 *  AxROM (Mapper 7)
 * ============================================================ */
static void axrom_update(nes_bus_t *bus)
{
    int total_32k = nes_prg_banks_count / 4;
    if (total_32k <= 0) return;

    int bank = g_mapper.axrom_bank % total_32k;
    int b8 = bank * 4;

    if (b8 + 3 >= nes_prg_banks_count) return;

    set_prg_8k(bus, 0x8000, nes_prg_banks[b8]);
    set_prg_8k(bus, 0xA000, nes_prg_banks[b8 + 1]);
    set_prg_8k(bus, 0xC000, nes_prg_banks[b8 + 2]);
    set_prg_8k(bus, 0xE000, nes_prg_banks[b8 + 3]);

    if (bus->ppu) {
        bus->ppu->mirroring = (g_mapper.axrom_bank & 0x10) ? 3 : 2;
    }
}

/* ============================================================
 *  MMC2 (Mapper 9) — Punch-Out
 * ============================================================ */
static int mmc2_chr_bank_count_4k(void)
{
    return nes_chr_banks_count / 4;
}

static int mmc2_selected_left_bank(void)
{
    return (g_mapper.mmc2_latch0 == 0xFD)
        ? g_mapper.mmc2_chr_fd0
        : g_mapper.mmc2_chr_fe0;
}

static int mmc2_selected_right_bank(void)
{
    return (g_mapper.mmc2_latch1 == 0xFD)
        ? g_mapper.mmc2_chr_fd1
        : g_mapper.mmc2_chr_fe1;
}

static void mmc2_apply_active_chr(nes_bus_t *bus)
{
    if (!bus || !bus->ppu || !nes_chr_is_rom) return;

    int total_4k = mmc2_chr_bank_count_4k();
    if (total_4k <= 0) return;

    int left = mmc2_selected_left_bank() % total_4k;
    int right = mmc2_selected_right_bank() % total_4k;

    set_chr_4k(bus, 0, left);
    set_chr_4k(bus, 1, right);
}

static void mmc2_apply_prg(nes_bus_t *bus)
{
    if (!bus || nes_prg_banks_count < 4) return;

    int total_8k = nes_prg_banks_count;
    int bank = g_mapper.mmc2_prg_bank % total_8k;

    set_prg_8k(bus, 0x8000, nes_prg_banks[bank]);
    set_prg_8k(bus, 0xA000, nes_prg_banks[total_8k - 3]);
    set_prg_8k(bus, 0xC000, nes_prg_banks[total_8k - 2]);
    set_prg_8k(bus, 0xE000, nes_prg_banks[total_8k - 1]);
}

static void mmc2_write(uint16_t addr, uint8_t value, nes_bus_t *bus)
{
    if (addr < 0xA000) return;

    switch (addr & 0xF000) {
        case 0xA000:
            /* $A000-$AFFF: switchable 8KB PRG at $8000-$9FFF. */
            g_mapper.mmc2_prg_bank = value & 0x0F;
            mmc2_apply_prg(bus);
            break;

        case 0xB000:
            /* latch0=FD CHR register */
            g_mapper.mmc2_chr_fd0 = value & 0x1F;
            mmc2_apply_active_chr(bus);
            break;

        case 0xC000:
            /* latch0=FE CHR register */
            g_mapper.mmc2_chr_fe0 = value & 0x1F;
            mmc2_apply_active_chr(bus);
            break;

        case 0xD000:
            /* latch1=FD CHR register */
            g_mapper.mmc2_chr_fd1 = value & 0x1F;
            mmc2_apply_active_chr(bus);
            break;

        case 0xE000:
            /* latch1=FE CHR register */
            g_mapper.mmc2_chr_fe1 = value & 0x1F;
            mmc2_apply_active_chr(bus);
            break;

        case 0xF000:
            /* MMC2: bit0 0=vertical, 1=horizontal. */
            if (bus->ppu) {
                bus->ppu->mirroring = (value & 1) ? 0 : 1;
            }
            break;

        default:
            break;
    }
}

static void mapper_mmc2_setup_impl(void)
{
    g_mapper.mmc2_prg_bank = 0;
    g_mapper.mmc2_chr_fd0 = 0;
    g_mapper.mmc2_chr_fe0 = 0;
    g_mapper.mmc2_chr_fd1 = 0;
    g_mapper.mmc2_chr_fe1 = 0;
    g_mapper.mmc2_latch0 = 0xFE;
    g_mapper.mmc2_latch1 = 0xFE;

    if (g_mapper_bus) {
        mmc2_apply_prg(g_mapper_bus);
        mmc2_apply_active_chr(g_mapper_bus);
    }
}

/* ============================================================
 *  M11 — Color Dreams
 *  PRG: 32K switchable (bits 0-3)  CHR: 8K switchable (bits 4-7)
 * ============================================================ */
static void m11_update(nes_bus_t *bus)
{
    set_prg_32k(bus, m11_prg);
    set_chr_8k_bank(bus, m11_chr);
}

/* ============================================================
 *  M34 — BNROM
 *  PRG: 32K switchable (bits 0-3)
 * ============================================================ */
static void m34_update(nes_bus_t *bus)
{
    set_prg_32k(bus, m34_prg);
}

/* ============================================================
 *  M66 — GNROM
 *  PRG: 32K switchable (bits 4-7)  CHR: 8K switchable (bits 0-3)
 * ============================================================ */
static void m66_update(nes_bus_t *bus)
{
    set_prg_32k(bus, m66_prg);
    set_chr_8k_bank(bus, m66_chr);
}

/* ============================================================
 *  M78.3 — Holy Diver IF-12
 *  bits 0-2: 16K PRG at $8000   bit 3: mirroring (1=V, 0=H)
 *  bits 4-7: 8K CHR bank
 * ============================================================ */
static void m78_update(nes_bus_t *bus)
{
    set_prg_16k_switch_low(bus, m78_prg);
    set_prg_last_16k(bus);
    set_chr_8k_bank(bus, m78_chr);
    if (bus->ppu) {
        bus->ppu->mirroring = m78_mirror ? 1 : 0;
    }
}

/* ============================================================
 *  M180 — UNROM Crazy (7408)
 *  Same PRG banking as M2 UNROM, different bus behavior.
 * ============================================================ */
/* Mapper 180 (UNROM'un tersi, Crazy Climber): $8000-$BFFF ILK 16K banka sabit, $C000-$FFFF yazilan banka gecer
 * (eskiden UNROM gibi eslenirdi: oyun bank yazinca sabit kodu $8000'den kayboluyor, CPU sifir dolu alanda kaliyordu). */
static void m180_update(nes_bus_t *bus)
{
    int total_16k = nes_prg_banks_count / 2;
    if (total_16k <= 0) return;
    int bank = g_mapper.unrom_bank % total_16k;
    set_prg_16k(bus, 0x8000, 0);
    set_prg_16k(bus, 0xC000, bank);
}

/* ============================================================
 *  Public API
 * ============================================================ */
static void mapper_map_prg_pages_impl(nes_bus_t *bus)
{
    if (!bus) return;
    mapper_tags_clear();   /* tam yeniden esleme: etiketler gecersiz */

    /* MMC2 PPU latch handler icin bus pointer'ini sakla. */
    g_mapper_bus = bus;

    switch (nes_last_mapper_loaded) {
        case 0:
            break;

        case 1:
            mmc1_update(bus);
            break;

        case 2:
            unrom_update(bus);
            break;

        case 3:
            cnrom_update(bus);
            break;

        case 4:
            mmc3_update(bus);
            break;

        case 7:
            axrom_update(bus);
            break;

        case 9:
            mmc2_apply_prg(bus);
            mmc2_apply_active_chr(bus);
            break;

        case 11:
            m11_update(bus);
            break;

        case 28:
            a53_apply(bus);
            break;

        case 34:
            m34_update(bus);
            break;

        case 66:
            m66_update(bus);
            break;

        case 78:
            m78_update(bus);
            break;

        case 180:
            m180_update(bus);
            break;

        default:
            break;
    }
}

static void mapper_write_impl(uint16_t addr, uint8_t value, nes_bus_t *bus)
{
    if (!bus) return;
    if (addr < 0x5000) return;

    /* mapper_map_prg_pages() normalde bunu zaten yapmis olur. */
    g_mapper_bus = bus;

    switch (nes_last_mapper_loaded) {
        case 1:
            if (addr >= 0x8000) mmc1_write(addr, value, bus);
            break;

        case 2:
            if (addr >= 0x8000) {
                g_mapper.unrom_bank = value & 0x0F;
                unrom_update(bus);
            }
            break;

        case 3:
            if (addr >= 0x8000) {
                g_mapper.cnrom_bank = value & 0x03;
                cnrom_update(bus);
            }
            break;

        case 4:
            mmc3_write(addr, value, bus);
            break;

        case 7:
            if (addr >= 0x8000) {
                g_mapper.axrom_bank = value & 0x1F;
                axrom_update(bus);
            }
            break;

        case 9:
            mmc2_write(addr, value, bus);
            break;

        case 11:
            if (addr >= 0x8000) {
                m11_prg = value & 0x0F;
                m11_chr = (value >> 4) & 0x0F;
                m11_update(bus);
            }
            break;

                        case 28:
            /* NESdev Action 53: $5000-$5FFF yazimi register secer (yalniz bit 7 ve bit 0: $00 CHR, $01 ic PRG, $80 kip,
             * $81 dis PRG); $8000-$FFFF yazimi secili registera yazar. Tek ekran aynalama kiplerinde (kip bit 1 = 0) $00/$01
             * yazimlarinin bit 4'u ekran sayfasini (kip bit 0) belirler. (Eskiden yalniz tam $5000/$8000 adresleri
             * taniniyordu: Streemerz menusu oyunu baslatamiyordu.) */
            if (addr >= 0x5000 && addr < 0x6000) {
                a53_select = (uint8_t)(value & 0x81);
            } else if (addr >= 0x8000) {
                switch (a53_select) {
                    case 0x00: a53_chr   = value; break;
                    case 0x01: a53_prg   = value; break;
                    case 0x80: a53_mode  = value; break;
                    case 0x81: a53_outer = value; break;
                    default: break;
                }
                if (a53_select <= 0x01 && !(a53_mode & 0x02))
                    a53_mode = (uint8_t)((a53_mode & ~0x01) | ((value >> 4) & 0x01));
                a53_apply(bus);
            }
            break;

        case 34:
            if (addr >= 0x8000) {
                m34_prg = value & 0x0F;
                m34_update(bus);
            }
            break;

        case 66:
            if (addr >= 0x8000) {
                m66_prg = (value >> 4) & 0x0F;
                m66_chr = value & 0x0F;
                m66_update(bus);
            }
            break;

        case 78:
            if (addr >= 0x8000) {
                m78_prg = value & 0x07;
                m78_mirror = (value >> 3) & 1;
                m78_chr = (value >> 4) & 0x0F;
                m78_update(bus);
            }
            break;

        case 180:
            if (addr >= 0x8000) {
                g_mapper.unrom_bank = value & 0x0F;
                m180_update(bus);
            }
            break;

        default:
            break;
    }
}
/* Genel giris: PRG pencereleri degistiyse 32 KB goruntuyu esle (bkz. nes_rom.c nes_prg_commit). */
void mapper_write(uint16_t addr, uint8_t value, nes_bus_t *bus)
{
    mapper_write_impl(addr, value, bus);
    if (nes_prg_dirty) nes_prg_commit();
    mapper_refresh_ppu_dep();
}

/* Genel giris: PRG pencereleri degistiyse 32 KB goruntuyu esle (bkz. nes_rom.c nes_prg_commit). */
void mapper_map_prg_pages(nes_bus_t *bus)
{
    mapper_map_prg_pages_impl(bus);
    if (nes_prg_dirty) nes_prg_commit();
    mapper_refresh_ppu_dep();
}

/* Genel giris: PRG pencereleri degistiyse 32 KB goruntuyu esle (bkz. nes_rom.c nes_prg_commit). */
void mapper_mmc2_setup(void)
{
    mapper_mmc2_setup_impl();
    if (nes_prg_dirty) nes_prg_commit();
    mapper_refresh_ppu_dep();
}