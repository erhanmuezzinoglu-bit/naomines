#include <naomi/video.h>
#include <naomi/audio.h>
#include <naomi/timer.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "fake6502.h"
#include "ppu.h"
#include "nes_input.h"
#include "nes_rom.h"
#include "nes_bus.h"
#include "rom_browser.h"
#include "emu_driver.h"
#include "debug_hud.h"
#include "apu.h"
#include "exp_config.h"

/* ppu.c: 0 = orijinal yol, 1 = hizli blit, 2 = yedek (yavas) yol. */
extern int ppu_blit_mode(void);

static fake6502_context cpu;
static ppu_t ppu;
static nes_input_t in;
static nes_bus_t bus;

#if EXP_ABLATE
int g_exp_mode = 0;
#endif

/* ------------------------------------------------------------------ */
/* Kalibrasyon testi: CPU hizi ve cache davranisi                      */
/* ------------------------------------------------------------------ */
#if EXP_BENCH
static volatile uint32_t bench_small[2048];      /*   8 KB: D-cache'e sigar    */
static volatile uint32_t bench_big[262144];      /*   1 MB: D-cache'e sigmaz   */
static volatile uint32_t bench_sink;
static unsigned long bench_alu_x10, bench_small_x10, bench_big_x10;   /* ns*10 / islem */

static unsigned long bench_ns_x10(uint64_t us, unsigned long iters)
{
    return (unsigned long)((us * 10000ULL) / iters);
}

static void bench_run(void)
{
    int h;
    uint64_t us;
    uint32_t x = 1;
    uint32_t s;

    /* 1) Saf ALU/dongu hizi: 2M iterasyon. */
    h = profile_start();
    for (uint32_t i = 0; i < 2000000u; i++) x = x * 1664525u + 1013904223u;
    us = profile_end(h);
    bench_sink = x;
    bench_alu_x10 = bench_ns_x10(us, 2000000ul);

    /* 2) 8 KB dizi, 1000 gecis: cache calisiyorsa hizli olmali. */
    s = 0;
    h = profile_start();
    for (int pass = 0; pass < 1000; pass++)
        for (int i = 0; i < 2048; i++) s += bench_small[i];
    us = profile_end(h);
    bench_sink = s;
    bench_small_x10 = bench_ns_x10(us, 2048ul * 1000ul);

    /* 3) 1 MB dizi, 32 bayt adimla (her erisim yeni cache satiri): ~2M erisim. */
    s = 0;
    h = profile_start();
    for (int pass = 0; pass < 64; pass++)
        for (int i = 0; i < 262144; i += 8) s += bench_big[i];
    us = profile_end(h);
    bench_sink = s;
    bench_big_x10 = bench_ns_x10(us, 32768ul * 64ul);
}

static void bench_draw(int y)
{
    video_draw_debug_text(
        8, y, rgb(150, 200, 255),
        "BENCH ns/islem  alu:%lu.%lu  8KB:%lu.%lu  1MB:%lu.%lu",
        bench_alu_x10 / 10, bench_alu_x10 % 10,
        bench_small_x10 / 10, bench_small_x10 % 10,
        bench_big_x10 / 10, bench_big_x10 % 10);
}
#endif

/* ------------------------------------------------------------------ */
/* Ablasyon olcumu                                                     */
/* ------------------------------------------------------------------ */
#if EXP_ABLATE
#define ABL_LOOPS 5       /* her modda olculen emulasyon dongusu sayisi */

static const char *const abl_name[EXP_NMODES] = {
    "base", "noAPU", "noSprPipe", "noLineRnd", "allOff",
    "noPPUstep", "bypassBus", "CPUonly", "dupFull", "dupMin"
};
static unsigned long abl_us[EXP_NMODES];      /* us / NES frame */
static int abl_valid[EXP_NMODES];
static uint64_t abl_acc;
static int abl_cnt;
static int abl_skip;

static void abl_draw(int y)
{
    char buf[512];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n,
                  "ABLASYON emu ms/frame (aktif: M%d)\n", g_exp_mode);
    for (int m = 0; m < EXP_NMODES; m++) {
        if (abl_valid[m]) {
            n += snprintf(buf + n, sizeof(buf) - n, "M%d %-10s %lu.%lu\n",
                          m, abl_name[m], abl_us[m] / 1000, (abl_us[m] / 100) % 10);
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "M%d %-10s --\n", m, abl_name[m]);
        }
    }
    video_draw_debug_text(8, y, rgb(120, 255, 160), "%s", buf);
}
#endif

/* ------------------------------------------------------------------ */
/* Olcum penceresi: SADECE ROM yukluyken ve autorun aciksken sayilir.  */
/* ------------------------------------------------------------------ */
#if EXP_PROFILE
#define PROF_WIN 8        /* ortalama penceresi (emulasyon dongusu sayisi) */

static uint64_t acc_emu, acc_aud, acc_draw, acc_hud, acc_wait, acc_wall;
static unsigned long avg_emu, avg_aud, avg_draw, avg_hud, avg_wait;
static unsigned long avg_nes_fps10;      /* NES fps * 10 */
static int prof_n;
static int prof_frame0;
static int prof_have;                    /* en az bir pencere tamamlandi mi */
#endif

#if EXP_PROFILE
static inline int  profile_start_if(void) { return profile_start(); }
static inline void profile_end_into(int h, uint64_t *acc) { *acc += profile_end(h); }
#else
static inline int  profile_start_if(void) { return 0; }
static inline void profile_end_into(int h, uint64_t *acc) { (void)h; (void)acc; }
#endif

/* Sesi hic calmadan at (turbo modunda ara framelerin sesi). */
#if EXP_TURBO_FRAMES > 1
static void discard_audio(void)
{
    static int16_t trash[1024];
    emu_driver_sync_apu();   /* tembel APU: bosaltmadan once guncelle */
    while (apu_drain_samples(&bus.apu, trash, 1024) == 1024) { }
}
#endif

int main(void)
{
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(30,30,40));

    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, 44100, 2048);

    nes_input_init(&in);
    ppu_init(&ppu);
    nes_bus_init(&bus, &ppu, &in);
    emu_driver_init(&cpu, &ppu, &in, &bus);

    rom_browser_init();

    int selected = 0;
    int rom_loaded = 0;
    int autorun = 0;
    int steps_per_frame = 29780;

#if EXP_BENCH
    bench_run();
#endif

#if EXP_NO_FILL
    /* video_display_on_vblank() arka tamponu arka plan rengiyle doldurur.
     * Bir kez cagirarak iki tamponu da dolu hale getir. */
    video_display_on_vblank();
#endif

    while (1)
    {
        int p;
        uint64_t t_emu = 0, t_aud = 0, t_hud = 0, t_draw = 0, t_wait = 0;
#if EXP_PROFILE
        const int frame_at_start = ppu.frame;
        const int loop_h = profile_start();
        int counted = 0;
#endif

        nes_input_poll(&in);

        if (!rom_loaded)
        {
            int cnt = rom_browser_count();
            if (cnt > 0) {
                if (in.edge.player1.up) selected = (selected + cnt - 1) % cnt;
                if (in.edge.player1.down) selected = (selected + 1) % cnt;

                if (in.edge.player1.start || in.edge.player1.button1) {
                    const char *name = rom_browser_name(selected);

                    if (name) {
                        char fullpath[128];
                        snprintf(fullpath, sizeof(fullpath), "rom://%s", name);

                        memset(bus.mem, 0, 0x0800);
                        rom_loaded = nes_rom_load_mapper0(fullpath, bus.mem, &ppu);

                        if (rom_loaded) {
                            emu_driver_reset_from_vector();
                            autorun = 0;
                            steps_per_frame = 29780;
                        }
                    }
                }
            }
        }
        else
        {
            /* ---- Emulasyon ---- */
            p = profile_start_if();
            if (in.edge.player1.button6) {
                for (int i = 0; i < 8; i++) emu_driver_step_1();
            }

            if (in.edge.player1.button4) autorun = !autorun;

            if (in.edge.player1.button5) {
                emu_driver_reset_from_vector();
            }

            if (autorun) {
                for (int f = 0; f < EXP_TURBO_FRAMES; f++) {
                    int cycles_in_frame = 0;
                    while (cycles_in_frame < steps_per_frame) {
                        uint32_t dc = emu_driver_step_1();
                        cycles_in_frame += dc;
                        if (dc == 0) break;
                    }
#if EXP_TURBO_FRAMES > 1
                    if (f < EXP_TURBO_FRAMES - 1) discard_audio();
#endif
                }
            }
            profile_end_into(p, &t_emu);

            /* ---- Ses ---- */
            p = profile_start_if();
            {
                static int16_t scratch[1024];
                uint32_t got;
                emu_driver_sync_apu();   /* tembel APU: bosaltmadan once guncelle */
                while ((got = apu_drain_samples(&bus.apu, scratch, 1024)) > 0) {
                    audio_write_mono_data(AUDIO_CHANNEL_LEFT,  scratch, got);
                    audio_write_mono_data(AUDIO_CHANNEL_RIGHT, scratch, got);
                    if (got < 1024) break;
                }
            }
            profile_end_into(p, &t_aud);

#if EXP_PROFILE
            counted = autorun;
#endif
        }

#if !EXP_NO_FILL
        video_fill_screen(rgb(30,30,40));
#endif

        if (!rom_loaded) {
            rom_browser_draw(selected);
#if EXP_BENCH
            bench_draw(452);
#endif
        } else {
            p = profile_start_if();
            debug_hud_draw(&cpu, autorun, steps_per_frame);
            profile_end_into(p, &t_hud);

            emu_driver_sync_ppu();   /* tembel PPU: cizimden once guncelle */

            p = profile_start_if();
            ppu_draw(&ppu);
            profile_end_into(p, &t_draw);

#if EXP_PROFILE
            /* Olcum satirlari (hud/draw olcumlerinin disinda). */
            video_draw_debug_text(
                8, 24, rgb(255, 230, 80),
                "emu:%lu aud:%lu draw:%lu hud:%lu wait:%lu us%s\n"
                "NES %lu.%lu fps  blit:%d nofill:%d turbo:%d  M%d",
                avg_emu, avg_aud, avg_draw, avg_hud, avg_wait,
                prof_have ? "" : " (olculuyor)",
                avg_nes_fps10 / 10, avg_nes_fps10 % 10,
                ppu_blit_mode(), EXP_NO_FILL, EXP_TURBO_FRAMES,
#if EXP_ABLATE
                g_exp_mode
#else
                0
#endif
                );
#endif
#if EXP_ABLATE
            abl_draw(292);
#endif
#if EXP_BENCH
            bench_draw(392);
#endif
        }

        p = profile_start_if();
        video_display_on_vblank();
        profile_end_into(p, &t_wait);

#if EXP_PROFILE
        {
            const uint64_t loop_us = profile_end(loop_h);

            if (!counted) {
                /* Menu / duraklatma / ROM yukleme: pencereyi sifirla. */
                prof_n = 0;
                acc_emu = acc_aud = acc_draw = acc_hud = acc_wait = acc_wall = 0;
#if EXP_ABLATE
                g_exp_mode = 0;
                abl_acc = 0; abl_cnt = 0; abl_skip = 1;
#endif
            } else {
                if (prof_n == 0) prof_frame0 = frame_at_start;
                acc_emu += t_emu; acc_aud += t_aud; acc_draw += t_draw;
                acc_hud += t_hud; acc_wait += t_wait; acc_wall += loop_us;
                prof_n++;

                if (prof_n >= PROF_WIN) {
                    int nes_frames = ppu.frame - prof_frame0;
                    if (nes_frames < 0) nes_frames = 0;

                    avg_emu  = (unsigned long)(acc_emu  / prof_n);
                    avg_aud  = (unsigned long)(acc_aud  / prof_n);
                    avg_draw = (unsigned long)(acc_draw / prof_n);
                    avg_hud  = (unsigned long)(acc_hud  / prof_n);
                    avg_wait = (unsigned long)(acc_wait / prof_n);
                    avg_nes_fps10 = acc_wall
                        ? (unsigned long)(((uint64_t)nes_frames * 10000000ULL) / acc_wall)
                        : 0;
                    prof_have = 1;

                    prof_n = 0;
                    acc_emu = acc_aud = acc_draw = acc_hud = acc_wait = acc_wall = 0;
                }

#if EXP_ABLATE
                if (abl_skip > 0) {
                    abl_skip--;                 /* mod degisiminden sonraki ilk dongu atilir */
                } else {
                    abl_acc += t_emu;
                    abl_cnt++;
                    if (abl_cnt >= ABL_LOOPS) {
                        abl_us[g_exp_mode] =
                            (unsigned long)(abl_acc / abl_cnt / EXP_TURBO_FRAMES);
                        abl_valid[g_exp_mode] = 1;
                        abl_acc = 0; abl_cnt = 0; abl_skip = 1;
                        g_exp_mode = (g_exp_mode + 1) % EXP_NMODES;
                    }
                }
#endif
            }
        }
#endif
    }

    return 0;
}