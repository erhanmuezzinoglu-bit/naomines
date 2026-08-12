#include <naomi/video.h>
#include <naomi/audio.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "fake6502.h"
#include "ppu.h"
#include "trace.h"
#include "nes_input.h"
#include "nes_rom.h"
#include "nes_bus.h"
#include "rom_browser.h"
#include "emu_driver.h"
#include "debug_hud.h"
#include "apu.h" // APU dosyasını dahil ediyoruz

/* -------------------------
    Global emulator state
   ------------------------- */
static fake6502_context cpu; // CPU durumu
static ppu_t ppu;            // PPU nesnesi
static nes_input_t in;       // Kullanıcı girdi sistemi
static trace_state_t tr;     // İzleme mekanizması
static nes_bus_t bus;        // Bus nesnesi

// RAM izleme adresleri
static uint16_t watch_addrs[] = { 0x00D2, 0x00D4, 0x0002, 0x0003 };
#define WATCH_COUNT ((int)(sizeof(watch_addrs) / sizeof(watch_addrs[0])))

int main(void)
{
    // Grafik sistemi başlatılır
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(30,30,40));

    /* === Phase 1: AICA + ringbuffer ===
     * 44100 Hz, 16-bit, stereo (mono kaynaktan iki kanala kopya).
     * 2048 sample ≈ 46ms latency, underflow toleransı için yeterli.
     * num_samples 4'e bölünebilir olmalı (libnaomi gereksinimi). */
    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, 44100, 2048);

    // Sistem modüllerini başlatıyoruz
    trace_init(&tr);
    nes_input_init(&in);
    ppu_init(&ppu);
    nes_bus_init(&bus, &ppu, &in, &tr); // NES Bus başlatılıyor
    emu_driver_init(&cpu, &ppu, &in, &tr, &bus); // Emülatör sürücüsü başlatılıyor

    rom_browser_init();

    // Değişkenler
    int selected = 0;
    int rom_loaded = 0;
    int autorun = 0; 
    int steps_per_frame = 29780; 
    int trace_scroll = 0;

    tr.enable = 1; // İzleme mekanizması etkinleştirildi

    // Ana emülatör döngüsü
    while (1)
    {
        // Kullanıcı inputlarını al ve değerlendir
        nes_input_poll(&in);

        if (!rom_loaded)
        {
            int cnt = rom_browser_count();
            if (cnt > 0) {
                // ROM seçim işlemleri
                if (in.edge.player1.up) selected = (selected + cnt - 1) % cnt;
                if (in.edge.player1.down) selected = (selected + 1) % cnt;

                // ROM yükleme işlemi
                if (in.edge.player1.start || in.edge.player1.button1) {
                    const char *name = rom_browser_name(selected);

                    if (name) {
                        char fullpath[128];
                        snprintf(fullpath, sizeof(fullpath), "rom://%s", name);

                        // ROM belleğini temizle ve yükle
                        memset(bus.mem, 0, 0x0800); 
                        rom_loaded = nes_rom_load_mapper0(fullpath, bus.mem, &ppu);

                        if (rom_loaded) {
                            // --- CPU RESET ---
                            emu_driver_reset_from_vector();

                            autorun = 0; 
                            steps_per_frame = 29780; 
                            trace_scroll = 0;
                            tr.head = 0;
                            tr.error_detected = 0;
                            memset(tr.rb, 0, sizeof(tr.rb));
                        }
                    }
                }
            }
        }
        else
        {
            // Kontroller ve emülatör işleyişi
            if (in.edge.player1.button6) {
                // Tek adım modunda işlem yapılır (button6 işlemi)
                for (int i = 0; i < 8; i++) {
                    emu_driver_step_1();
                }
            }

            // Oto çalışma durumu kontrolü
            if (in.edge.player1.button4) autorun = !autorun;

            // Emülatör yeniden kurulum -> resetleme işlemi
            if (in.edge.player1.button5) { 
                emu_driver_reset_from_vector(); 
                tr.error_detected = 0;
            }

            // Performans ve izleme ayarları
            if (in.edge.player1.left)  steps_per_frame = emu_driver_clamp_int(steps_per_frame - 500, 1, 100000);
            if (in.edge.player1.right) steps_per_frame = emu_driver_clamp_int(steps_per_frame + 500, 1, 100000);
            if (in.edge.player1.up)    trace_scroll++;
            if (in.edge.player1.down)  trace_scroll--;

            // Oto çalışma döngüsü (autorun)
            if (autorun) {
                int cycles_in_frame = 0;
                while (cycles_in_frame < steps_per_frame) {
                    uint32_t dc = emu_driver_step_1();
                    cycles_in_frame += dc; 
                    if (dc == 0) break;
                }
            }

            /* === Phase 1: APU sample'larını AICA ringbuffer'a yaz ===
             * Mono kaynak → her iki kanala aynı veri.
             * Frame başına ~735 sample emit edilir (44100/60). */
            {
                static int16_t scratch[1024];
                uint32_t got;
                while ((got = apu_drain_samples(&bus.apu, scratch, 1024)) > 0) {
                    audio_write_mono_data(AUDIO_CHANNEL_LEFT,  scratch, got);
                    audio_write_mono_data(AUDIO_CHANNEL_RIGHT, scratch, got);
                    if (got < 1024) break;
                }
            }
        }

        // İzleme ayarları
        {
            uint32_t available = (tr.head < TRACE_SIZE) ? tr.head : TRACE_SIZE;
            int max_back = (int)available - 10;
            if (max_back < 0) max_back = 0;
            trace_scroll = emu_driver_clamp_int(trace_scroll, 0, max_back);
        }

        // Görselleştirme
        video_fill_screen(rgb(30,30,40));

        if (!rom_loaded) {
            rom_browser_draw(selected);
        } else {
            debug_hud_draw(&cpu, emu_driver_instr_count(), &ppu, &bus, &tr, 
                           autorun, steps_per_frame, trace_scroll, watch_addrs, WATCH_COUNT);
            ppu_draw(&ppu);
        }

        video_display_on_vblank();
    }

    return 0;
}