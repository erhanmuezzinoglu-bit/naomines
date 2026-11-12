#include <naomi/video.h>
#include <naomi/audio.h>
#include <naomi/timer.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "fake6502.h"
#include "ppu.h"
#include "nes_input.h"
#include "nes_rom.h"
#include "nes_bus.h"
#include "rom_browser.h"
#include "emu_driver.h"
#include "apu.h"
#include "exp_config.h"

/* ppu.c: 0 = orijinal yol, 1 = hizli blit, 2 = yedek (yavas) yol. */
extern int ppu_blit_mode(void);

static fake6502_context cpu;
static ppu_t ppu;
static nes_input_t in;
static nes_bus_t bus;



/* ------------------------------------------------------------------ */
/* Kalibrasyon testi: CPU hizi ve cache davranisi                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Ablasyon olcumu                                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Olcum penceresi: SADECE ROM yukluyken ve autorun aciksken sayilir.  */
/* ------------------------------------------------------------------ */
#define BG_COLOR rgb(30, 30, 40)   /* arka plan rengi (bkz. g_bg_full) */
#define PROF_WIN 8        /* ortalama penceresi (emulasyon dongusu sayisi) */

static uint64_t acc_emu, acc_aud, acc_draw, acc_hud, acc_wait, acc_wall, acc_in, acc_txt;
static unsigned long avg_emu, avg_aud, avg_draw, avg_hud, avg_wait, avg_in, avg_txt, avg_wall, avg_nfr10;
static unsigned long acc_nfr;
#define MS1(v) (unsigned long)(v) / 1000, ((unsigned long)(v) / 100) % 10
static unsigned long avg_nes_fps10;      /* NES fps * 10 */
static int prof_n;
static int prof_frame0;
static int prof_have;                    /* en az bir pencere tamamlandi mi */

/* Son karede bos dongu atlamasiyla gecilen cycle (HUD: ff:). */
static unsigned long ff_last_frame;

/* Tani satirlari NES goruntusunun ALTINDA (goruntu y ~36-276, x ~360-616; satirlar 78 karakteri gecmez). */
#define HUD_Y(i) (288 + 12 * (i))


static inline int  profile_start_if(void) { return profile_start(); }
static inline void profile_end_into(int h, uint64_t *acc) { *acc += profile_end(h); }

/* Sesi hic calmadan at (turbo modunda ara framelerin sesi). */

/* Menuden ikinci (ve sonraki) ROM yuklemesi: acilistaki ile ayni "soguk baslangic" dizisi (onceki oyundan hicbir CPU/PPU/APU/mapper
 * durumu kalmaz). Test: onceki oyundan sonra bu dizi, taze surecte yuklemeyle ayni durum hash'ini verir. */
static int load_rom_cold(const char *fullpath)
{
    memset(&cpu, 0, sizeof cpu);
    nes_input_init(&in);
    ppu_init(&ppu);
    ppu_clear_screen();
    nes_bus_init(&bus, &ppu, &in);
    emu_driver_init(&cpu, &ppu, &in, &bus);
    memset(bus.mem, 0, 0x0800);
    if (!nes_rom_load_mapper0(fullpath, bus.mem, &ppu)) return 0;
    emu_driver_reset_from_vector();
    return 1;
}

#if EXP_BENCH
/* ---- Otomatik olcum (EXP_BENCH) ----
 * Her oyun: soguk yukleme, girdi yok, EXP_BENCH_WARM dongu isinma + EXP_BENCH_LOOPS dongu olcum. Dongu govdesi normal oynatmayla
 * ayni (kare atlama, hizalama, butce); vblank beklenmez (yalniz her 16 dongude bir ilerleme ekrani). Olculenler:
 *   emu  : emulasyon suresi / dongu (ana ekrandaki "emu" ile ayni olcum; EXP_DIAG_PROF'ta yalniz kip 0 donguleri)
 *   max  : 8 donguluk pencerelerin en yuksek ortalamasi (agir sahne)
 *   is   : emu + ses bosaltma + PPU senkronu + ppu_draw (vblank beklemesi haric tum is) + 0.9 ms girdi
 *   fps  : is'ten tahmini NES fps (60 Hz ekranda dongu (1+atla) kare; dongu suresi 16.7 ms'nin katina yuvarlanir)
 *   ff   : atlanan cycle yuzdesi
 * EXP_DIAG_PROF'ta ek sutunlar: ppu, apu, cizim, cpu+sur (ana ekrandaki olcum satiriyla ayni formul). */
static const char *const bench_roms[] = {
    "Gradius.nes", "Metroid.nes", "Excitebike.nes", "Castlevania.nes", "Mega Man 2.nes",
    "Super Mario Bros. 3.nes", "Crystal Mines.nes", "Kirby's Adventure.nes", "Legend of Zelda, The.nes",
    "Super Mario Bros .nes", "Castlevania III - Dracula's Curse.nes", "Crazy Climber.nes", "Deadly Towers.nes",
    "Duck Hunt.nes", "Gargoyle's Quest II.nes", "Holy Diver.nes", "Little Sisyphus.nes", "Marble Madness.nes",
    "Prince of Persia.nes", "Punch-Out!!.nes", "Streemerz.nes",
};
#define BENCH_N ((int)(sizeof(bench_roms) / sizeof(bench_roms[0])))
/* Tus betigi (EXP_BENCH_INPUT): dongu araliginda [bas, bit) basili tutulan NES tuslari. Bitler: A=1 B=2 SELECT=4 START=8 YUKARI=16
 * ASAGI=32 SOL=64 SAG=128. Betikler TAHMINDIR (gercek ROM'larla denenmedi): amac acilis menulerini gecip oynanisa girmek.
 * Bir oyun menude kaliyorsa yalniz o satiri degistirin. Emulasyon belirlenimci: ayni betik her derlemede ayni sahneyi verir. */
typedef struct { short from, to; unsigned char keys; } bench_key_t;
#define BK_A 1
#define BK_B 2
#define BK_SEL 4
#define BK_ST 8
#define BK_U 16
#define BK_D 32
#define BK_L 64
#define BK_R 128
static const bench_key_t bench_script[][16] = {
    /* 1 Gradius     */ { {60, 63, BK_ST}, {120, 700, BK_R | BK_A}, {0, 0, 0} },
    /* 2 Metroid     */ { {60, 63, BK_ST}, {100, 103, BK_ST}, {170, 700, BK_R}, {0, 0, 0} },
    /* 3 Excitebike  */ { {60, 63, BK_ST}, {100, 103, BK_ST}, {130, 700, BK_A}, {0, 0, 0} },
    /* 4 Castlevania */ { {60, 63, BK_ST}, {130, 700, BK_R}, {0, 0, 0} },
    /* 5 Mega Man 2  */ { {60, 63, BK_ST}, {100, 103, BK_ST}, {140, 143, BK_ST}, {250, 253, BK_R}, {280, 283, BK_ST}, {330, 333, BK_R}, {360, 363, BK_ST}, {0, 0, 0} },
    /* 6 SMB3        */ { {120, 123, BK_ST}, {160, 163, BK_ST}, {250, 253, BK_R}, {270, 273, BK_U}, {290, 293, BK_A}, {330, 333, BK_R}, {350, 353, BK_U}, {370, 373, BK_A}, {420, 700, BK_R}, {0, 0, 0} },
    /* 7 Crystal Min */ { {60, 63, BK_ST}, {0, 0, 0} },
    /* 8 Kirby       */ { {120, 123, BK_ST}, {170, 173, BK_A}, {210, 213, BK_A}, {250, 253, BK_ST}, {300, 700, BK_R}, {0, 0, 0} },
    /* 9 Zelda       */ { {60, 63, BK_ST}, {100, 103, BK_ST}, {200, 700, BK_R}, {0, 0, 0} },
    /*10 SMB         */ { {60, 63, BK_ST}, {150, 700, BK_R}, {200, 210, BK_A}, {260, 270, BK_A}, {320, 330, BK_A}, {380, 390, BK_A}, {440, 450, BK_A}, {500, 510, BK_A}, {0, 0, 0} },
    /*11 Castlev. III*/ { {60, 63, BK_ST}, {0, 0, 0} },
    /*12 Crazy Climb */ { {260, 263, BK_ST}, {320, 700, BK_U}, {0, 0, 0} },
    /*13 Deadly Tow. */ { {60, 63, BK_ST}, {120, 123, BK_ST}, {180, 183, BK_ST}, {240, 700, BK_R}, {0, 0, 0} },
    /*14 Duck Hunt   */ { {60, 63, BK_ST}, {0, 0, 0} },
    /*15 Gargoyle II */ { {260, 263, BK_ST}, {300, 303, BK_ST}, {340, 343, BK_A}, {370, 373, BK_A}, {400, 403, BK_A}, {430, 433, BK_A}, {460, 463, BK_A}, {490, 493, BK_A}, {520, 523, BK_A}, {0, 0, 0} },
    /*16 Holy Diver  */ { {100, 103, BK_ST}, {160, 163, BK_ST}, {220, 223, BK_ST}, {280, 700, BK_R}, {0, 0, 0} },
    /*17 L. Sisyphus */ { {120, 123, BK_ST}, {180, 183, BK_ST}, {240, 700, BK_R}, {0, 0, 0} },
    /*18 Marble Mad. */ { {100, 103, BK_ST}, {140, 143, BK_ST}, {200, 203, BK_D}, {206, 209, BK_D}, {212, 215, BK_D}, {218, 221, BK_R}, {224, 227, BK_R}, {230, 233, BK_R}, {236, 239, BK_R}, {242, 245, BK_R}, {248, 251, BK_R}, {258, 261, BK_A}, {280, 283, BK_A}, {330, 700, BK_D}, {0, 0, 0} },
    /*19 Pr. Persia  */ { {160, 163, BK_ST}, {320, 323, BK_ST}, {360, 363, BK_ST}, {420, 700, BK_R}, {0, 0, 0} },
    /*20 Punch-Out   */ { {120, 123, BK_ST}, {160, 163, BK_ST}, {200, 203, BK_ST}, {240, 243, BK_ST}, {300, 700, BK_A}, {0, 0, 0} },
    /*21 Streemerz   */ { {120, 123, BK_ST}, {200, 203, BK_ST}, {240, 243, BK_ST}, {280, 283, BK_ST}, {320, 323, BK_ST}, {470, 473, BK_ST}, {510, 513, BK_ST}, {550, 700, BK_R}, {0, 0, 0} },
};

typedef char bench_script_satir_sayisi[(sizeof(bench_script) / sizeof(bench_script[0]) == BENCH_N) ? 1 : -1];

static void bench_keys(int gi, int L)
{
    unsigned k = 0;
#if EXP_BENCH_INPUT
    for (int i = 0; i < 16 && bench_script[gi][i].to; i++)
        if (L >= bench_script[gi][i].from && L < bench_script[gi][i].to) k |= bench_script[gi][i].keys;
#else
    (void)gi; (void)L;
#endif
    in.input_valid = 1;
    in.buttons.player1.button1 = (k & BK_A) != 0;
    in.buttons.player1.button2 = (k & BK_B) != 0;
    in.buttons.player1.button3 = (k & BK_SEL) != 0;
    in.buttons.player1.start   = (k & BK_ST) != 0;
    in.buttons.player1.right   = (k & BK_R) != 0;
    in.buttons.player1.up      = (k & BK_U) != 0;
    in.buttons.player1.left    = (k & BK_L) != 0;
    in.buttons.player1.down    = (k & BK_D) != 0;
}
typedef struct { int ok, mapper; unsigned long emu, max, work, fps10, ffpct; long ppu, spr, sat, apu, drw, cpu; } bench_row_t;
static bench_row_t bench_res[BENCH_N];
#if EXP_BENCH_PC
extern uint32_t g_pch[65536];
extern int g_pch_on;
#define BENCH_PC_N 5
typedef struct { uint16_t pc[BENCH_PC_N]; uint8_t pct[BENCH_PC_N]; uint8_t b[BENCH_PC_N][3]; } bench_pc_t;
static bench_pc_t bench_pc[BENCH_N];
#endif
/* Her oyunun olcum sonundaki karesinden 42x40 kucuk resim (6'da 1; sonuc ekraninda: hangi sahne olculdu). */
#define BTH_W 42
#define BTH_H 40
static uint16_t bench_thumb[BENCH_N][BTH_H][BTH_W];
/* Emulatorun destekledigi mapper'lar (mapper.c); digerleri olculmez, tabloda belirtilir. */
static int bench_mapper_ok(int m)
{
    static const int ok[] = { 0, 1, 2, 3, 4, 7, 9, 11, 28, 34, 66, 78, 180 };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) if (ok[i] == m) return 1;
    return 0;
}

static void bench_short_name(char *dst, const char *src, int n)
{
    int i = 0;
    for (; src[i] && src[i] != '.' && i < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void bench_one(int gi)
{
    bench_row_t *r = &bench_res[gi];
    char path[128], nm[16];
    memset(r, 0, sizeof *r);
    snprintf(path, sizeof path, "rom://%s", bench_roms[gi]);
    bench_short_name(nm, bench_roms[gi], 12);
    if (!load_rom_cold(path)) return;
    r->mapper = nes_last_mapper_loaded;
    if (!bench_mapper_ok(r->mapper)) { r->ok = 2; return; }
    r->ok = 1;
    int spf = 29780;
#if EXP_FRAME_ALIGN
    int align_adj = 0;
#endif
    uint64_t acc_e = 0, acc_w = 0, win = 0, acc_m[5] = {0, 0, 0, 0, 0}, acc_fh_ = 0, acc_fv_ = 0;
    unsigned long n_e = 0, n_win = 0, n_m[5] = {0, 0, 0, 0, 0}, ff_tot = 0, cyc_tot = 0, n_d = 0;
    uint64_t acc_d = 0;
    const int total = EXP_BENCH_WARM + EXP_BENCH_LOOPS;
#if EXP_BENCH_PC
    memset(g_pch, 0, sizeof g_pch);
#endif
    for (int L = 0; L < total; L++) {
        const int meas = L >= EXP_BENCH_WARM;
        bench_keys(gi, L);
#if EXP_BENCH_PC
        g_pch_on = meas;
#endif
        int mode = 0;
#if EXP_DIAG_PROF
        mode = meas ? (L % 5) : 0;   /* 0 taban, 1 +golge PPU, 2 +golge APU, 3 golge PPU sprite'siz, 4 golge PPU satir cizimsiz */
        emu_driver_prof_set_mode(mode);
#endif
        uint64_t t_fh = 0, t_fv = 0;
        const unsigned long ff0 = g_ff_cycles_total;
        const uint32_t ck0 = cpu.emu.clockticks;
        const int h_emu = profile_start();
#if EXP_BENCH_SKIP > 0
        for (int f = 0; f < 1 + EXP_BENCH_SKIP; f++) {
            ppu_set_render_skip(f < EXP_BENCH_SKIP);
#else
        for (int f = 0; f < EXP_TURBO_FRAMES; f++) {
#endif
            int cif = 0;
            const int fp = profile_start();
#if EXP_FRAME_ALIGN
            const int budget = spf + (f == 0 ? align_adj : 0);
#else
            const int budget = spf;
#endif
            emu_driver_ff_set_stop(cpu.emu.clockticks + budget);
            while (cif < budget) { uint32_t dc = emu_driver_step_1(); cif += dc; if (dc == 0) break; }
            emu_driver_ff_set_stop(0);
            if (ppu_get_render_skip()) t_fh += profile_end(fp); else t_fv += profile_end(fp);
        }
#if EXP_BENCH_SKIP > 0
        ppu_set_render_skip(0);
#endif
        const uint64_t te = profile_end(h_emu);
#if EXP_DIAG_PROF
        emu_driver_prof_set_mode(0);
#endif
        const int h_rest = profile_start();
        {   /* ses: bosalt ve at (normal oynatmadaki apu_drain_samples isi) */
            static int16_t scratch[1024];
            uint32_t got;
            emu_driver_sync_apu();
            while ((got = apu_drain_samples(&bus.apu, scratch, 1024)) > 0) { if (got < 1024) break; }
        }
        emu_driver_sync_ppu();
#if EXP_FRAME_ALIGN
        {
            int e = ppu.scanline - EXP_ALIGN_LINE;
            if (e >= 131) e -= 262;
            if (e < -131) e += 262;
            int adj = -((e * 341 + ppu.cycle) / 3);
            if (adj > 7000) adj = 7000;
            if (adj < -7000) adj = -7000;
            align_adj = adj;
        }
#endif
        const uint64_t tr = profile_end(h_rest);
        const int show = (L % 16) == 0;
        if (show) {   /* ppu_draw her 16 dongude bir: suresi ayri olculur, ortalamasi 'is'e eklenir */
            const int h_d = profile_start();
            ppu_draw(&ppu);
            const uint64_t td = profile_end(h_d);
            if (meas) { acc_d += td; n_d++; }
        }
        if (meas) {
            acc_m[mode] += te; n_m[mode]++;
            if (mode == 0) {
                acc_e += te; n_e++; acc_w += te + tr;
                acc_fh_ += t_fh; acc_fv_ += t_fv;
                win += te; n_win++;
                if (n_win == 8) { const unsigned long a = (unsigned long)(win / 8); if (a > r->max) r->max = a; win = 0; n_win = 0; }
                ff_tot += g_ff_cycles_total - ff0; cyc_tot += cpu.emu.clockticks - ck0;
            }
        }
        if (show) {
#if EXP_BG_MANUAL
            video_fill_box(0, 20, 639, 34, BG_COLOR);
#endif
            video_draw_debug_text(8, 24, rgb(255, 230, 80), "OLCUM %d/%d  %s  %s %d/%d", gi + 1, BENCH_N, nm,
                                  meas ? "olcum" : "isinma", meas ? L - EXP_BENCH_WARM : L, meas ? EXP_BENCH_LOOPS : EXP_BENCH_WARM);
            video_display_on_vblank();
        }
    }
#if EXP_BENCH_PC
    g_pch_on = 0;
    {   /* en sik BENCH_PC_N komut adresi (yuzde: tum olcum komutlarina gore) + o adresteki 3 bayt (o anki PRG gorunumu) */
        uint64_t tot = 0;
        for (int i = 0; i < 65536; i++) tot += g_pch[i];
        bench_pc_t *bp = &bench_pc[gi];
        memset(bp, 0, sizeof *bp);
        for (int k = 0; k < BENCH_PC_N; k++) {
            uint32_t best = 0; int bi = -1;
            for (int i = 0; i < 65536; i++) if (g_pch[i] > best) { best = g_pch[i]; bi = i; }
            if (bi < 0 || !tot) break;
            bp->pc[k] = (uint16_t)bi;
            bp->pct[k] = (uint8_t)((uint64_t)best * 100u / tot);
            for (int j = 0; j < 3; j++) bp->b[k][j] = nes_bus_peek(&bus, (uint16_t)(bi + j));
            g_pch[bi] = 0;
        }
    }
#endif
    {   /* son karenin kucuk resmi (4'te 1) */
        static uint16_t full[256 * 240];
        emu_driver_sync_ppu();
        ppu_copy_screen(full);
        for (int y = 0; y < BTH_H; y++) for (int x = 0; x < BTH_W; x++) bench_thumb[gi][y][x] = full[(y * 6) * 256 + x * 6];
    }
    if (n_e) {
        r->emu = (unsigned long)(acc_e / n_e);
        r->work = (unsigned long)(acc_w / n_e) + (n_d ? (unsigned long)(acc_d / n_d) : 0) + 900;
        r->ffpct = cyc_tot ? (unsigned long)((uint64_t)ff_tot * 100u / cyc_tot) : 0;
    }
    {
#if EXP_BENCH_SKIP > 0
        const unsigned fpl = 1 + EXP_BENCH_SKIP;
#else
        const unsigned fpl = EXP_TURBO_FRAMES;
#endif
        unsigned vb = (unsigned)((r->work + 16666) / 16667);   /* dongu kac vblank surer (yukari yuvarla) */
        if (vb < 1) vb = 1;
        r->fps10 = (unsigned long)(fpl * 600u / vb);
        if (r->fps10 > 600) r->fps10 = 600;
    }
#if EXP_DIAG_PROF
    if (n_m[0] && n_m[1] && n_m[2] && n_m[3] && n_m[4]) {
        const long b = (long)(acc_m[0] / n_m[0]);
        r->ppu = (long)(acc_m[1] / n_m[1]) - b;
        r->spr = (long)(acc_m[1] / n_m[1]) - (long)(acc_m[3] / n_m[3]);   /* sprite degerlendirme payi (PPU icinde) */
        r->sat = (long)(acc_m[1] / n_m[1]) - (long)(acc_m[4] / n_m[4]);   /* satir cizimi payi (gizli satir yolu, PPU icinde) */
        r->apu = (long)(acc_m[2] / n_m[2]) - b;
        r->drw = (long)((acc_fv_ - acc_fh_) / n_m[0]);
        r->cpu = b - r->ppu - r->apu - (r->drw > 0 ? r->drw : 0);
    }
#else
    (void)acc_m; (void)n_m; (void)acc_fh_; (void)acc_fv_;
#endif
}

#define BMS(v) (long)(v) / 1000, (labs((long)(v)) / 100) % 10
static void bench_run(void)
{
    for (int gi = 0; gi < BENCH_N; gi++) bench_one(gi);
    /* sonuc ekrani: START ile cik. Ekran sabit oldugu icin iki tampona birer kez cizilir, sonra tampon DEGISTIRILMEZ
     * (her vblank'teki degisim + libnaomi'nin arka plan doldurmasi emulatorlerde kararmaya yol aciyordu); girdi
     * ~60 Hz'te zamanlayiciyla beklenerek okunur. */
    nes_input_poll(&in);
    for (int pass = 0; pass < 2; pass++) {
#if EXP_BG_MANUAL
        video_fill_screen(BG_COLOR);
#endif
        int y = 24;
        video_draw_debug_text(8, y, rgb(255, 230, 80), "OLCUM SONUCU  atla:%d  %d+%d dongu  tus:%s  (ms/dongu)  START: menu",
                              EXP_BENCH_SKIP, EXP_BENCH_WARM, EXP_BENCH_LOOPS, EXP_BENCH_INPUT ? "betik" : "yok");
        y += 16;
#if EXP_DIAG_PROF
        video_draw_debug_text(8, y, rgb(200, 255, 160), "oyun           emu   max    is  fps  ff%%  ppu (spr sat)  apu ciz  cpu");
#else
        video_draw_debug_text(8, y, rgb(200, 255, 160), "oyun           emu   max    is   fps  ff%%");
#endif
        y += 12;
        for (int gi = 0; gi < BENCH_N; gi++, y += 12) {
            const bench_row_t *r = &bench_res[gi];
            char nm[16];
            bench_short_name(nm, bench_roms[gi], 12);
            if (!r->ok) { video_draw_debug_text(8, y, rgb(255, 120, 120), "%2d %-11.11s  (romfs'te yok)", gi + 1, nm); continue; }
            if (r->ok == 2) { video_draw_debug_text(8, y, rgb(255, 120, 120), "%2d %-11.11s  (mapper %d desteklenmiyor)", gi + 1, nm, r->mapper); continue; }
#if EXP_DIAG_PROF
            video_draw_debug_text(8, y, rgb(230, 230, 230), "%2d %-11.11s%2ld.%ld %2ld.%ld %2ld.%ld %2lu.%lu %3lu %2ld.%ld (%ld.%ld %ld.%ld) %ld.%ld %ld.%ld %2ld.%ld",
                gi + 1, nm, BMS(r->emu), BMS(r->max), BMS(r->work), r->fps10 / 10, r->fps10 % 10, r->ffpct,
                BMS(r->ppu), BMS(r->spr), BMS(r->sat), BMS(r->apu), BMS(r->drw), BMS(r->cpu));
#else
            video_draw_debug_text(8, y, rgb(230, 230, 230), "%2d %-11.11s%2ld.%ld %2ld.%ld %2ld.%ld  %2lu.%lu %3lu",
                gi + 1, nm, BMS(r->emu), BMS(r->max), BMS(r->work), r->fps10 / 10, r->fps10 % 10, r->ffpct);
#endif
        }
        y += 4;
#if !EXP_BENCH_PC
        video_draw_debug_text(8, y, rgb(160, 220, 255), "emu: emulasyon  max: en agir 8 dongu  is: vblank beklemesi haric tum is");
        video_draw_debug_text(8, y + 10, rgb(160, 220, 255), "fps: is'ten tahmini NES fps  ff: atlanan cycle  betik: main.c");
#endif
#if !EXP_BENCH_PC
        {   /* olculen sahnelerin kucuk resimleri (tablo sirasiyla; satir basina 14) */
            for (int gi = 0; gi < BENCH_N; gi++) {
                const int tx = 8 + (gi % 14) * 45, ty = y + 28 + (gi / 14) * (BTH_H + 12);
                video_draw_debug_text(tx, ty, rgb(255, 230, 80), "%d", gi + 1);
                if (bench_res[gi].ok != 1) continue;
                if (!video_is_vertical() && video_depth() == 2) {   /* hizli yol: 1555 dogrudan tampona (cizim suresi kisa) */
                    uint16_t *fb = (uint16_t *)video_framebuffer();
                    const unsigned w = video_width();
                    for (int yy = 0; yy < BTH_H; yy++) memcpy(&fb[(unsigned)(ty + 10 + yy) * w + (unsigned)tx], bench_thumb[gi][yy], BTH_W * 2);
                } else
                for (int yy = 0; yy < BTH_H; yy++) for (int xx = 0; xx < BTH_W; xx++) {
                    const uint16_t v = bench_thumb[gi][yy][xx];
                    const int r5 = (v >> 10) & 0x1F, g5 = (v >> 5) & 0x1F, b5 = v & 0x1F;
                    video_draw_pixel(tx + xx, ty + 10 + yy, rgb((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2)));
                }
            }
        }
#endif
#if EXP_BENCH_PC
        {   /* sicak komut adresleri: adres:yuzde bayt1bayt2bayt3 (komut uzunlugu kadar bayt anlamlidir) */
            /* 21 oyun: aciklama satirlarinin yerine, 8 piksel aralikla (kucuk resimler bu kipte cizilmez) */
            int py = y;
            for (int gi = 0; gi < BENCH_N; gi++, py += 8) {
                if (bench_res[gi].ok != 1) continue;
                const bench_pc_t *bp = &bench_pc[gi];
                char line[128]; int n = snprintf(line, sizeof line, "%d", gi + 1);
                for (int k = 0; k < BENCH_PC_N && n < (int)sizeof line - 16; k++)
                    n += snprintf(line + n, sizeof line - n, " %04X:%02u %02X%02X%02X", bp->pc[k], bp->pct[k], bp->b[k][0], bp->b[k][1], bp->b[k][2]);
                video_draw_debug_text(8, py, rgb(230, 230, 230), "%s", line);
            }
        }
#endif
        video_display_on_vblank();
    }
    for (;;) {
        nes_input_poll(&in);
        if (in.edge.player1.start) break;
        timer_wait(16667);
    }
}
#undef BMS
#endif

/* ---- Arka plan: libnaomi'nin video_set_background_color'u HER vblank'te 640x480 tamponun tamamini (600 KB) doldurur.
 * Oyunda NES goruntusu her karede ayni bolgeye yeniden cizildiginden bu gereksizdir: arka plan yalniz menude ve gecislerde
 * (iki tampon icin 2 kare) tam doldurulur, oyunda yalniz metin satirlarinin kutulari temizlenir. ---- */
#if EXP_BG_MANUAL
static int g_bg_full = 2;   /* > 0: sonraki karelerde ekranin tamami doldurulur (her tampon icin bir kare) */
#endif

/* ---- Duvar saati (us): profile_* tutamaclari tek seferliktir; surekli saat = biten araliklarin toplami ---- */
static uint64_t now_us(void)
{
    static int h = -1;
    static uint64_t acc = 0;
    if (h >= 0) acc += profile_end(h);
    h = profile_start();
    return acc;
}

/* ---- Ses cikisi: tampon dolulugu + hiz denetimi (EXP_AUDIO_RATECTL) ----
 * Doluluk (yeni ornegin calinmasina kalan ornek) EXP_AUDIO_HWPOS'ta AICA okuma konumundan olculur; aksi halde muhasebeyle
 * tahmin edilir (yazilanlar eklenir, gecen sure * 44100 cikarilir; yazim eksik kabul edilirse doluluk = kapasite, 0'in altina
 * inerse 0). Ornekler, doluluk hedefe yaklassin diye en fazla +-%1 oraninda dogrusal aradegerle yeniden orneklenir (faz ve
 * son ornek cagrilar arasinda tasinir: kesinti yok). Doluluk her parti yaziminden HEMEN ONCE (en dusuk noktasinda) olculur. */
#define AUD_CAP (EXP_AUDIO_RING - 512)          /* libnaomi RINGBUFFER_SAFETY_SIZE kadar yer bos tutulur */
static int32_t aud_fill;                         /* tahmini doluluk (ornek) */
static uint64_t aud_last_us;                     /* son guncelleme ani */
static uint32_t aud_frac;                        /* tuketim hesabinin kesir artigi (us * 44100 mod 1e6) */
static uint32_t aud_phase;                       /* yeniden ornekleyici fazi (16.16) */
static int16_t aud_prev;                         /* bir onceki partinin son ornegi */
static unsigned long aud_under, aud_over;        /* bosalma / tasma sayaclari (HUD) */
static int32_t aud_tgt = EXP_AUDIO_TARGET;       /* son etkin hedef (HUD) */

#if EXP_AUDIO_HWPOS
/* libnaomi'nin ic AICA komutu (genel sembol, audio.c): halka tamponun OKUMA konumunu (ornek) dondurur. Yazma konumunu biz
 * izleriz (kayit aninda 512 + yazilan ornek; libnaomi 16 bitte ornekleri ciftler halinde yazar). Gecersiz yanit gelirse
 * (farkli libnaomi surumu) tahmine geri donulur. */
extern uint32_t __audio_exchange_command(uint32_t command, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4);
#define AUD_REQ_RINGBUFFER_POSITION 0x100Bu
#define AUD_CHANNEL_LEFT 1u
static uint32_t aud_written;                     /* kayittan beri yazilan ornek (yazma konumu = 512 + bu) */
static int aud_hw_ok = 1;
static int32_t aud_hw_raw(void)                  /* (yazma - okuma) mod halka; gecersizse -1 */
{
    const uint32_t rp = __audio_exchange_command(AUD_REQ_RINGBUFFER_POSITION, AUD_CHANNEL_LEFT, 0, 0, 0);
    if (rp >= (uint32_t)EXP_AUDIO_RING) { aud_hw_ok = 0; return -1; }
    const uint32_t wp = ((512u + aud_written) & ~1u) % (uint32_t)EXP_AUDIO_RING;
    return (int32_t)((wp + (uint32_t)EXP_AUDIO_RING - rp) % (uint32_t)EXP_AUDIO_RING);
}
#endif
/* Ogrenilen etkin kapasite (doluluk birimiyle): libnaomi bir yazimi eksik kabul ettiginde (tampon dolu) o anki doluluk.
 * Kurulu libnaomi surumunun guvenlik payi farkliysa ya da yazma konumu muhasebesi sabit bir farkla kaymissa, hedef doluluk
 * buna gore kendiliginden asagi cekilir (hedef + parti kapasiteyi asmasin). */
static int32_t aud_cap = AUD_CAP;

static void aud_consume_to_now(void)
{
    const uint64_t t = now_us();
    const uint64_t dt = t - aud_last_us;
    aud_last_us = t;
    const uint64_t num = dt * 44100u + aud_frac;
    aud_frac = (uint32_t)(num % 1000000u);
    aud_fill -= (int32_t)(num / 1000000u);       /* tahmin (donanim konumu yoksa tek kaynak; varsa tutarlilik denetimi) */
#if EXP_AUDIO_HWPOS
    if (aud_hw_ok) {
        /* doluluk = yeni yazilacak ornegin calinmasina kalan ornek = (yazma - okuma) mod halka. Okuma yazmayi gecmisse
         * (bosalma) AICA halkadaki eski veriyi calmaya devam eder; bu deger o durumda da dogrudur (buyuk cikar). Yasal
         * yazimla kapasiteyi asamaz: asiyorsa bosalma olmustur (sayac). */
        const int32_t hf = aud_hw_raw();
        if (hf >= 0) {
            if (hf > AUD_CAP) aud_under++;
            aud_fill = hf;
            return;
        }
    }
#endif
    if (aud_fill < 0) { aud_fill = 0; aud_under++; }
}

static void aud_full(unsigned lost)   /* libnaomi yazimin bir kismini kabul etmedi: tampon dolu */
{
    aud_over += lost;
#if EXP_AUDIO_HWPOS
    if (aud_hw_ok) {
        const int32_t hf = aud_hw_raw();
        if (hf >= 1024) { aud_cap = hf; aud_fill = hf; return; }
    }
#endif
    aud_fill = aud_cap;
}

static void aud_write(int16_t *st, unsigned n)   /* st: araya girmis stereo (L,R), n cift */
{
    unsigned done = 0;
    while (done < n) {
        const int w = audio_write_stereo_data(st + 2 * done, n - done);
        if (w <= 0) { aud_full(n - done); return; }   /* dolu: kalan atilir */
        done += (unsigned)w;
        aud_fill += w;
#if EXP_AUDIO_HWPOS
        aud_written += (uint32_t)w;
#endif
        if (done < n) { aud_full(n - done); return; }
    }
}

static void aud_reset(void)   /* oyun yuklenince: hedefin yarisi kadar sessizlik ile baslat */
{
    static int16_t z[2 * 256];
    memset(z, 0, sizeof z);
    aud_last_us = now_us(); aud_frac = 0; aud_fill = 0; aud_phase = 0; aud_prev = 0;
    for (int k = 0; k < EXP_AUDIO_TARGET / 2 / 256; k++) aud_write(z, 256);
}

/* in: mono ornekler (n). Hiz denetimli cikis halkaya stereo yazilir. */
static void aud_push(const int16_t *in, unsigned n)
{
    static int16_t st[2 * 4096];
    aud_consume_to_now();
    if (n == 0) return;
    /* Doluluk hedefin cok ustundeyse (bosalma sonrasi AICA eski veriyi caliyor ya da uzun bir duraklama): bu parti atlanir;
     * yeniden ornekleme (+-%1) bunu ancak saniyelerde eritirdi. Okuma konumu yeni veriye ~(doluluk - hedef) ornekte ulasir. */
    if (aud_fill > aud_cap) { aud_over += n; aud_prev = in[n - 1]; return; }
    /* etkin hedef: en fazla EXP_AUDIO_TARGET; hedef + bu parti + pay ogrenilen kapasiteyi asmasin */
    int32_t tgt = aud_cap - (int32_t)n - 512;
    if (tgt > EXP_AUDIO_TARGET) tgt = EXP_AUDIO_TARGET;
    if (tgt < 512) tgt = 512;
    aud_tgt = tgt;
#if EXP_AUDIO_RATECTL
    /* oran = cikis/giris = 1 + d, d = 0,01 * (hedef - doluluk) / hedef, [-0,01, 0,01] araliginda; adim = 1/oran (16.16) */
    int32_t e = tgt - aud_fill;
    if (e > tgt) e = tgt;
    if (e < -tgt) e = -tgt;
    /* adim ~= 65536 * (1 - d): d = 0,01 * e / hedef  ->  adim = 65536 - 656 * e / hedef */
    const uint32_t step = (uint32_t)(65536 - (656 * e) / tgt);
    /* x[0] = onceki partinin son ornegi, x[1..n] = yeni ornekler; konum [0, n) araliginda ilerler */
    unsigned m = 0;
    uint32_t pos = aud_phase;
    while ((pos >> 16) < n && m < 4096u) {
        const unsigned i = pos >> 16;
        const int32_t a0 = i ? in[i - 1] : aud_prev;
        const int32_t a1 = in[i];
        const int32_t fr = (int32_t)((pos & 0xFFFFu) >> 1);          /* 15 bit: (a1 - a0) * fr 32 bite sigar */
        const int16_t v = (int16_t)(a0 + (((a1 - a0) * fr) >> 15));
        st[2 * m] = v; st[2 * m + 1] = v; m++;
        pos += step;
    }
    aud_phase = ((pos >> 16) >= n) ? pos - ((uint32_t)n << 16) : 0u;   /* cikis siniri (4096) dolduysa faz sifirlanir */
    aud_prev = in[n - 1];
    aud_write(st, m);
#else
    unsigned m = n < 4096u ? n : 4096u;
    for (unsigned i = 0; i < m; i++) { st[2 * i] = in[i]; st[2 * i + 1] = in[i]; }
    aud_write(st, m);
#endif
}

/* Ust iki bilgi satiri (ms dokumu + fps/ses). */
/* Ekran bilgisi: varsayilan olarak yalniz sol ust kosede (NES goruntusunun solundaki bosluk, x < 64) NES fps.
 * EXP_HUD_DETAIL 1: ek olarak iki tani satiri (ms dokumu, kare/dongu ve ses tamponu) goruntunun ustune yazilir. */
static void hud_top(int nfr)
{
    video_draw_debug_text(8, 8, rgb(255, 230, 80), "%lu.%lu", avg_nes_fps10 / 10, avg_nes_fps10 % 10);
#if EXP_HUD_DETAIL
    video_draw_debug_text(72, 8, rgb(160, 220, 255), "ms dongu %lu.%lu emu %lu.%lu ses %lu.%lu ciz %lu.%lu bekle %lu.%lu",
                          MS1(avg_wall), MS1(avg_emu), MS1(avg_aud), MS1(avg_draw), MS1(avg_wait));
    video_draw_debug_text(72, 20, rgb(255, 230, 80), "kare/dongu %d  ses %ld/%ld/%ld  bos %lu tas %lu",
                          nfr, (long)aud_fill, (long)aud_tgt, (long)aud_cap, aud_under, aud_over);
#else
    (void)nfr;
#endif
}

int main(void)
{
    video_init(VIDEO_COLOR_1555);
#if EXP_BG_MANUAL
    video_fill_screen(BG_COLOR);   /* arka plan rengi kayitli DEGIL (bkz. g_bg_full) */
#else
    video_set_background_color(BG_COLOR);   /* libnaomi her vblank'te tum tamponu doldurur */
#endif

    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, 44100, EXP_AUDIO_RING);
    aud_reset();

    nes_input_init(&in);
    ppu_init(&ppu);
    nes_bus_init(&bus, &ppu, &in);
    emu_driver_init(&cpu, &ppu, &in, &bus);

    rom_browser_init();

    int selected = 0;
    int rom_loaded = 0;
    int autorun = 0;
    int loaded_once = 0;     /* ilk yukleme acilis durumundan; sonrakiler soguk baslangic */
    uint64_t start_hold_us = 0;   /* Start'in basili kaldigi sure (us): menuye donus */
    uint64_t last_loop_us = 0;    /* bir onceki dongu karesinin suresi (us) */
    int last_nfr = 1;             /* son dongude calistirilan NES karesi (HUD) */
    int steps_per_frame = 29780;
#if EXP_FRAME_ALIGN
    /* Kare hizalama: dongunun ilk NES karesinin butcesine eklenen duzeltme (CPU cycle); dongu sonu PPU satiri EXP_ALIGN_LINE'a oturur. */
    int align_adj = 0;
#endif


    /* iki tamponu da arka plan rengiyle doldur */
    video_display_on_vblank();
#if EXP_BG_MANUAL
    video_fill_screen(BG_COLOR);
#endif

#if EXP_BENCH
    bench_run();
#if EXP_BG_MANUAL
    g_bg_full = 2;
#endif
    loaded_once = 1;   /* sonraki menu yuklemesi soguk baslangic */
    ppu_clear_screen();
#endif

    while (1)
    {
        int p;
        uint64_t t_emu = 0, t_aud = 0, t_hud = 0, t_draw = 0, t_wait = 0, t_in = 0, t_txt = 0;
        const int frame_at_start = ppu.frame;
        const int loop_h = profile_start();
        int counted = 0;

        p = profile_start_if();
#if EXP_INPUT_PERIOD > 1
        {
            /* maple/JVS okumasi ~0.9 ms: her EXP_INPUT_PERIOD karede bir okunur; aradaki karelerde onceki
             * dugme durumu korunur, kenar (edge) bilgisi sifirlanir. */
            static unsigned in_frame;
            if ((in_frame++ % (unsigned)EXP_INPUT_PERIOD) == 0) nes_input_poll(&in);
            else memset(&in.edge, 0, sizeof(in.edge));
        }
#else
        nes_input_poll(&in);
#endif
        profile_end_into(p, &t_in);

        if (!rom_loaded)
        {
            int cnt = rom_browser_count();
            if (cnt > 0) {
                if (in.edge.player1.left) selected = (selected + cnt - 1) % cnt;    /* sol: onceki oyun */
                if (in.edge.player1.right) selected = (selected + 1) % cnt;         /* sag: sonraki oyun */

                if (in.edge.player1.start || in.edge.player1.button1) {
                    const char *name = rom_browser_name(selected);

                    if (name) {
                        char fullpath[128];
                        snprintf(fullpath, sizeof(fullpath), "rom://%s", name);

                        if (loaded_once) {
                            rom_loaded = load_rom_cold(fullpath);
                        } else {
                            memset(bus.mem, 0, 0x0800);
                            rom_loaded = nes_rom_load_mapper0(fullpath, bus.mem, &ppu);
                            if (rom_loaded) emu_driver_reset_from_vector();
                        }

                        if (rom_loaded) {
                            aud_reset();
                            loaded_once = 1;
                            autorun = EXP_AUTORUN_ON_LOAD;   /* secilen oyun B4'e basmadan calisir */
                            start_hold_us = 0;
                            steps_per_frame = 29780;
                        }
                    }
                }
            }
        }
        else
        {
            /* ---- Emulasyon ---- */
#if EXP_MENU_HOLD_MS > 0
            /* START EXP_MENU_HOLD_MS milisaniye BASILI kalirsa ana menuye doner (uyari yok; tus onceden basili olsa da sayilir). */
            if (in.buttons.player1.start) {
                start_hold_us += last_loop_us;
                if (start_hold_us >= (uint64_t)EXP_MENU_HOLD_MS * 1000ULL) {
                    rom_loaded = 0;
                    autorun = 0;
                    start_hold_us = 0;
                }
            } else {
                start_hold_us = 0;
            }
#endif
            const unsigned long ff0 = g_ff_cycles_total;
            p = profile_start_if();
            if (in.edge.player1.button6) {
                for (int i = 0; i < 8; i++) emu_driver_step_1();
            }

            if (in.edge.player1.button4) autorun = !autorun;

            /* B3: PPU sprite modu (dogruluk <-> HIZLI, bit-exact degil; bkz. exp_config.h EXP_FAST_PPU). */
            if (in.edge.player1.button3) ppu_set_fast_sprites(!ppu_get_fast_sprites());

            if (in.edge.player1.button5) {
                emu_driver_reset_from_vector();
            }

            if (autorun) {
#if EXP_FRAME_SKIP > 0
                /* KARE ATLAMA: dongu basina nfr NES karesi; yalniz sonuncusu cizilir (oncekiler gizli kare). Ses hepsinin.
                 * EXP_AUTO_SKIP: nfr = bir onceki dongunun surdugu vblank sayisi (en az 1, en fazla 1 + EXP_FRAME_SKIP). */
#if EXP_AUTO_SKIP
                int nfr = (int)((last_loop_us + 8333u) / 16683u);
                if (nfr < 1) nfr = 1;
                if (nfr > 1 + EXP_FRAME_SKIP) nfr = 1 + EXP_FRAME_SKIP;
#else
                const int nfr = 1 + EXP_FRAME_SKIP;
#endif
                last_nfr = nfr;
                for (int f = 0; f < nfr; f++) {
                    ppu_set_render_skip(f < nfr - 1);
#else
                for (int f = 0; f < EXP_TURBO_FRAMES; f++) {
#endif
                    int cycles_in_frame = 0;
                    /* Bos dongu atlamasi (EXP_IDLE_FF) kare butcesini asmasin. */
#if EXP_FRAME_ALIGN
                    const int budget = steps_per_frame + (f == 0 ? align_adj : 0);
#else
                    const int budget = steps_per_frame;
#endif
                    emu_driver_ff_set_stop(cpu.emu.clockticks + budget);
                    while (cycles_in_frame < budget) {
                        uint32_t dc = emu_driver_step_1();
                        cycles_in_frame += dc;
                        if (dc == 0) break;
                    }
                    emu_driver_ff_set_stop(0);
                }
#if EXP_FRAME_SKIP > 0
                ppu_set_render_skip(0);   /* gizli kare modu dongu disina tasmasin (menu, duraklatma, reset) */
#endif
            }
            profile_end_into(p, &t_emu);
            ff_last_frame = g_ff_cycles_total - ff0;

            /* ---- Ses ---- */
            p = profile_start_if();
            {
                static int16_t scratch[4096];
                uint32_t got, tot = 0;
                emu_driver_sync_apu();   /* tembel APU: bosaltmadan once guncelle */
                while (tot < 4000u && (got = apu_drain_samples(&bus.apu, scratch + tot, 4000u - tot)) > 0) tot += got;
                aud_push(scratch, tot);
            }
            profile_end_into(p, &t_aud);

            counted = autorun;
        }


#if EXP_BG_MANUAL
        if (!rom_loaded || g_bg_full > 0) {
            video_fill_screen(BG_COLOR);
            if (g_bg_full > 0) g_bg_full--;
            if (!rom_loaded) g_bg_full = 2;   /* oyuna geciste iki tampon da temizlensin */
        } else {
            video_fill_box(0, 4, 63, 16, BG_COLOR);         /* sol ust fps kutusu (goruntunun solunda) */
        }
#endif
        if (!rom_loaded) {
            rom_browser_draw(selected);
        } else {
            p = profile_start_if();
            profile_end_into(p, &t_hud);

            emu_driver_sync_ppu();   /* tembel PPU: cizimden once guncelle */
#if EXP_FRAME_ALIGN
            /* Dongu sonu VBL bolgesine (EXP_ALIGN_LINE) otursun: boylece her dongu tam bir NES karesinin 0..239 satirlarini
             * icerir (yirtilma yok; kare atlamada gizli kare tam 240 satiri atlar). Yalniz dongu sinirini kaydirir; emulasyon
             * cycle'a bagli oldugu icin sonuc degismez. */
            if (autorun) {
                int e = ppu.scanline - EXP_ALIGN_LINE;
                if (e >= 131) e -= 262;
                if (e < -131) e += 262;
                int adj = -((e * 341 + ppu.cycle) / 3);
                if (adj > 7000) adj = 7000;
                if (adj < -7000) adj = -7000;
                align_adj = adj;
            }
#endif

            p = profile_start_if();
            ppu_draw(&ppu);
            profile_end_into(p, &t_draw);

            p = profile_start_if();
            hud_top(last_nfr);
            /* ppu.c baska bir EXP_FRAME_SKIP degeriyle derlenmisse (eski .o) kare atlama piksel uretimini ATLAMAZ: uyar. */
            if (ppu_built_frame_skip != EXP_FRAME_SKIP)
                video_draw_debug_text(8, HUD_Y(6), rgb(255, 80, 80), "UYARI: ppu.c EXP_FRAME_SKIP=%d ile derlenmis, main.c %d: make clean",
                                      ppu_built_frame_skip, EXP_FRAME_SKIP);
            profile_end_into(p, &t_txt);
        }

        p = profile_start_if();
        video_display_on_vblank();
#if !EXP_BG_MANUAL
        if (rom_loaded) {
            /* Tampon degisiminden HEMEN sonra yeni arka tampona son kareyi ve ust satirlari yeniden ciz: libnaomi bu tamponu
             * arka plan rengiyle yeni doldurdu ve aksi halde dongu sonuna (emulasyon bitene) kadar bos kalirdi. Bos tamponun
             * gorunmesi (emulatorlerde gozlenen kararma) boylece onlenir; dongu sonunda yeni kare yine cizilir.
             * (EXP_BG_MANUAL'da gerekmez: tampon temizlenmez, onceki kareyi tutar.) */
            ppu_draw(&ppu);
            hud_top(last_nfr);
        }
#endif
        profile_end_into(p, &t_wait);

        {
            const uint64_t loop_us = profile_end(loop_h);
            last_loop_us = loop_us;

            if (!counted) {
                /* Menu / duraklatma / ROM yukleme: pencereyi sifirla. */
                prof_n = 0;
                acc_emu = acc_aud = acc_draw = acc_hud = acc_wait = acc_wall = acc_in = acc_txt = 0; acc_nfr = 0;
            } else {
                if (prof_n == 0) prof_frame0 = frame_at_start;
                acc_emu += t_emu; acc_aud += t_aud; acc_draw += t_draw;
                acc_hud += t_hud; acc_wait += t_wait; acc_wall += loop_us; acc_nfr += (unsigned long)last_nfr;
                acc_in += t_in; acc_txt += t_txt;
                prof_n++;

                if (prof_n >= PROF_WIN) {
                    int nes_frames = ppu.frame - prof_frame0;
                    if (nes_frames < 0) nes_frames = 0;

                    avg_emu  = (unsigned long)(acc_emu  / prof_n);
                    avg_wall = (unsigned long)(acc_wall / prof_n);
                    avg_nfr10 = acc_nfr * 10u / (unsigned long)prof_n; acc_nfr = 0;
                    avg_aud  = (unsigned long)(acc_aud  / prof_n);
                    avg_draw = (unsigned long)(acc_draw / prof_n);
                    avg_hud  = (unsigned long)(acc_hud  / prof_n);
                    avg_wait = (unsigned long)(acc_wait / prof_n);
                    avg_in   = (unsigned long)(acc_in   / prof_n);
                    avg_txt  = (unsigned long)(acc_txt  / prof_n);
                    avg_nes_fps10 = acc_wall
                        ? (unsigned long)(((uint64_t)nes_frames * 10000000ULL) / acc_wall)
                        : 0;
                    prof_have = 1;

                    prof_n = 0;
                    acc_emu = acc_aud = acc_draw = acc_hud = acc_wait = acc_wall = acc_in = acc_txt = 0;
                }

            }
        }
    }

    return 0;
}