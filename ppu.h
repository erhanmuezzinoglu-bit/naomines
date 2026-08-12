#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPU_SCREEN_W 256
#define PPU_SCREEN_H 240
#define PPU_INTERNAL_FB_W 512
#define PPU_INTERNAL_FB_H 240

typedef struct {
    uint8_t framebuffer[PPU_INTERNAL_FB_W * PPU_INTERNAL_FB_H];
    int scanline;
    int cycle;
    int frame;

    uint8_t reg_ctrl;    /* $2000 */
    uint8_t reg_mask;    /* $2001 */
    uint8_t reg_status;  /* $2002 */

    /* PPU Dahili Veri Latch'i (PPU Open Bus) */
    uint8_t open_bus;
    uint32_t open_bus_decay_timer; 

    uint16_t v;
    uint16_t t;
    uint8_t  x;
    uint8_t  w;

    uint8_t vram_read_buffer;
    uint8_t oam_addr;      /* $2003 */
    uint8_t oam[256];      /* $2004 */
    uint8_t vram[0x4000];
    uint8_t palette_ram[32];
    uint8_t mirroring;
    uint8_t nmi_requested;
    uint8_t dirty_all;

    bool vbl_suppress;

    /* Debug Sayaçları */
    uint32_t dbg_nmi_sent; 
} ppu_t;

void ppu_init(ppu_t *ppu);
void ppu_reset(ppu_t *ppu);
void ppu_step_cycles(ppu_t *ppu, uint32_t cycles);
void ppu_oam_dma(ppu_t *ppu, const uint8_t page[256]);
void ppu_draw(const ppu_t *ppu);
uint8_t ppu_cpu_read(ppu_t *ppu, uint16_t addr);
void ppu_cpu_write(ppu_t *ppu, uint16_t addr, uint8_t val);
uint16_t ppu_core_vram_mask(uint16_t a, uint8_t mirroring);
uint8_t  ppu_core_palette_mirror_index(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif