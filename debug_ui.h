#ifndef DEBUG_UI_H
#define DEBUG_UI_H

#include <stdint.h>
#include <naomi/video.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CPU Trace geçmişini ekrana çizer.
 * Artık opcode ve halt bilgilerini de içerir.
 */
void debug_draw_trace(int x, int y, int lines, int scroll_back,
                      const void *trace_state,      /* trace_state_t* pointer cast edilir */
                      const uint8_t *mem_ram);      /* CPU RAM/Bus pointer */

/**
 * Belirli RAM adreslerindeki değerleri canlı izler.
 */
void debug_draw_watch_ram(int x, int y,
                          const uint8_t mem_64k[0x10000],
                          const uint16_t *watch_addrs,
                          int watch_count);

/**
 * PPU Register durumlarını (Scroll, Address, Control) detaylı gösterir.
 */
void debug_draw_watch_ppu_regs(int x, int y,
                               uint8_t ppu2000, uint8_t ppu2001, uint8_t ppu2002_last,
                               uint8_t ppu2005_a, uint8_t ppu2005_b,
                               uint8_t ppu2006_hi, uint8_t ppu2006_lo,
                               uint8_t ppu2007);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UI_H */