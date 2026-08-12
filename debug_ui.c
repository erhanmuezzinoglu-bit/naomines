#include "debug_ui.h"
#include "trace.h"
#include "opc.h"
#include <stdio.h>

static int clamp_int_local(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Güncellenmiş Trace Çizici: Artık Illegal Opcode ve Accuracy uyarılarını vurguluyor
void debug_draw_trace(int x, int y, int lines, int scroll_back, const void *trace_state, const uint8_t *mem_ram)
{
    const trace_state_t *t = (const trace_state_t *)trace_state;
    uint32_t available = (t->head < TRACE_SIZE) ? t->head : TRACE_SIZE;
    if (available == 0) return;

    // 'halted' yerine 'error_detected' kullanıyoruz. Accuracy testlerini bozmadan sadece uyarı verir.[cite: 5]
    if (t->error_detected) {
        video_draw_debug_text(x, y - 20, rgb(255, 50, 50), "!!! ACCURACY ALERT: %s !!!", trace_halt_reason_str(t->halt_reason));
    } else {
        video_draw_debug_text(x, y - 20, rgb(50, 255, 50), "SYSTEM RUNNING (OBSERVER MODE)");
    }

    int max_back = (int)available - lines;
    if (max_back < 0) max_back = 0;
    scroll_back = clamp_int_local(scroll_back, 0, max_back);

    uint32_t start = t->head - (uint32_t)lines - (uint32_t)scroll_back;

    for (int i = 0; i < lines; i++) {
        uint32_t idx = (start + (uint32_t)i) % TRACE_SIZE;
        const trace_evt_t *e = &t->rb[idx];

        // Renk Mantığı: Bilinmeyen IO ise Turuncu, Hata PC'si ise Kırmızı, normal ise Gri[cite: 5]
        color_t c = rgb(200, 200, 200);
        if (e->unknown) c = rgb(255, 120, 80);
        if (t->error_detected && e->pc == t->halt_pc) c = rgb(255, 50, 50);

        char disasmbuf[64];
        disassemble_6502(mem_ram, e->pc, disasmbuf, sizeof(disasmbuf));

        char short_disasmbuf[32];
        snprintf(short_disasmbuf, sizeof(short_disasmbuf), "%-20s", disasmbuf); 

        // Snapshot'ta opcode verisi ve CPU register durumları çizilir[cite: 5]
        video_draw_debug_text(x, y + i * 18, c,
            "%s [%02X] A:%02X X:%02X Y:%02X P:%02X SP:%02X%s",
            short_disasmbuf,
            e->opcode,
            e->a, e->x, e->y, e->p, e->sp,
            e->unknown ? " !" : ""
        );
    }
}

void debug_draw_watch_ram(int x, int y,
                          const uint8_t mem_64k[0x10000],
                          const uint16_t *watch_addrs,
                          int watch_count)
{
    video_draw_debug_text(x, y, rgb(160,220,255), "RAM WATCH:");
    for (int i = 0; i < watch_count; i++) {
        uint16_t a = watch_addrs[i];
        uint8_t v = mem_64k[a];
        video_draw_debug_text(x, y + 18 + i * 18, rgb(200,200,200), "$%04X = %02X", a, v);
    }
}

void debug_draw_watch_ppu_regs(int x, int y,
                               uint8_t ppu2000, uint8_t ppu2001, uint8_t ppu2002_last,
                               uint8_t ppu2005_a, uint8_t ppu2005_b,
                               uint8_t ppu2006_hi, uint8_t ppu2006_lo,
                               uint8_t ppu2007)
{
    video_draw_debug_text(x, y, rgb(160,220,255), "--- PPU REGISTER WATCH ---");
    video_draw_debug_text(x, y + 18, rgb(200,200,200),
        "$2000:%02X  $2001:%02X  $2002(last):%02X",
        ppu2000, ppu2001, ppu2002_last);
    
    // Scroll ve Adres registerlarını daha belirgin yazalım
    video_draw_debug_text(x, y + 36, rgb(200,200,200),
        "$2005(Scroll): %02X, %02X  $2006(Addr): %02X, %02X",
        ppu2005_a, ppu2005_b,
        ppu2006_hi, ppu2006_lo);
}