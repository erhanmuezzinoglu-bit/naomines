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



/* ------------------------------------------------------------------ */
/* Kalibrasyon testi: CPU hizi ve cache davranisi                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Ablasyon olcumu                                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Olcum penceresi: SADECE ROM yukluyken ve autorun aciksken sayilir.  */
/* ------------------------------------------------------------------ */
#define PROF_WIN 8        /* ortalama penceresi (emulasyon dongusu sayisi) */

static uint64_t acc_emu, acc_aud, acc_draw, acc_hud, acc_wait, acc_wall, acc_in, acc_txt;
static unsigned long avg_emu, avg_aud, avg_draw, avg_hud, avg_wait, avg_in, avg_txt;
static unsigned long avg_nes_fps10;      /* NES fps * 10 */
static int prof_n;
static int prof_frame0;
static int prof_have;                    /* en az bir pencere tamamlandi mi */

/* Son karede bos dongu atlamasiyla gecilen cycle (HUD: ff:). */
static unsigned long ff_last_frame;

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
    int loaded_once = 0;     /* ilk yukleme acilis durumundan; sonrakiler soguk baslangic */
    uint64_t start_hold_us = 0;   /* Start'in basili kaldigi sure (us): menuye donus */
    uint64_t last_loop_us = 0;    /* bir onceki dongu karesinin suresi (us) */
    int steps_per_frame = 29780;


    /* video_display_on_vblank() arka tamponu arka plan rengiyle doldurur.
     * Bir kez cagirarak iki tamponu da dolu hale getir. */
    video_display_on_vblank();

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
                if (in.edge.player1.up) selected = (selected + cnt - 1) % cnt;
                if (in.edge.player1.down) selected = (selected + 1) % cnt;

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
                /* KARE ATLAMA: dongu basina (1 + EXP_FRAME_SKIP) NES karesi; yalniz sonuncusu cizilir (oncekiler gizli kare). Ses hepsinin. */
                for (int f = 0; f < 1 + EXP_FRAME_SKIP; f++) {
                    ppu_set_render_skip(f < EXP_FRAME_SKIP);
#else
                for (int f = 0; f < EXP_TURBO_FRAMES; f++) {
#endif
                    int cycles_in_frame = 0;
                    /* Bos dongu atlamasi (EXP_IDLE_FF) kare butcesini asmasin. */
                    emu_driver_ff_set_stop(cpu.emu.clockticks + steps_per_frame);
                    while (cycles_in_frame < steps_per_frame) {
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

            counted = autorun;
        }


        if (!rom_loaded) {
            rom_browser_draw(selected);
        } else {
            p = profile_start_if();
            profile_end_into(p, &t_hud);

            emu_driver_sync_ppu();   /* tembel PPU: cizimden once guncelle */

            p = profile_start_if();
            ppu_draw(&ppu);
            profile_end_into(p, &t_draw);

            p = profile_start_if();
            video_draw_debug_text(8, 24, rgb(255, 230, 80), "NES %lu.%lu fps  emu:%lu.%lums in:%lu.%lums  ff:%lu  ppu:%s",
                                  avg_nes_fps10 / 10, avg_nes_fps10 % 10,
                                  avg_emu / 1000, (avg_emu / 100) % 10, avg_in / 1000, (avg_in / 100) % 10,
                                  ff_last_frame, ppu_get_fast_sprites() ? "HIZLI" : "dogru");
            profile_end_into(p, &t_txt);
        }

        p = profile_start_if();
        video_display_on_vblank();
        profile_end_into(p, &t_wait);

        {
            const uint64_t loop_us = profile_end(loop_h);
            last_loop_us = loop_us;

            if (!counted) {
                /* Menu / duraklatma / ROM yukleme: pencereyi sifirla. */
                prof_n = 0;
                acc_emu = acc_aud = acc_draw = acc_hud = acc_wait = acc_wall = acc_in = acc_txt = 0;
            } else {
                if (prof_n == 0) prof_frame0 = frame_at_start;
                acc_emu += t_emu; acc_aud += t_aud; acc_draw += t_draw;
                acc_hud += t_hud; acc_wait += t_wait; acc_wall += loop_us;
                acc_in += t_in; acc_txt += t_txt;
                prof_n++;

                if (prof_n >= PROF_WIN) {
                    int nes_frames = ppu.frame - prof_frame0;
                    if (nes_frames < 0) nes_frames = 0;

                    avg_emu  = (unsigned long)(acc_emu  / prof_n);
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