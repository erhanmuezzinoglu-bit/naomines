#include "ppu.h"
#include "nes_rom.h"
#include "emu_driver.h"
#include <naomi/video.h>
#include <string.h>
#include <stdbool.h>

static void render_scanline(ppu_t *ppu);

static color_t screen_buffer[256 * 240];

static const uint8_t nintendo_power_up_palette[32] = {
    0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 
    0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
    0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 
    0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08
};

uint16_t ppu_core_vram_mask(uint16_t a, uint8_t mirroring) {
    a &= 0x3FFF;
    if (a >= 0x2000 && a <= 0x3EFF) {
        uint16_t addr = (a - 0x2000) & 0x0FFF;
        if (mirroring == 1) return 0x2000 | (addr & 0x07FF); // Vertical
        else return 0x2000 | (addr & 0x03FF) | ((addr & 0x0800) >> 1); // Horizontal
    }
    return a;
}

uint8_t ppu_core_palette_mirror_index(uint8_t idx) {
    idx &= 0x1F;
    if ((idx & 0x03) == 0x00 && idx >= 0x10) idx &= 0x0F;
    return idx;
}

static inline color_t nes_color_to_1555(uint8_t nes_color) {
    static const uint32_t palette_rgb[64] = {
        0x545454, 0x001e74, 0x081090, 0x300088, 0x440064, 0x5c0030, 0x540400, 0x3c1800,
        0x202a00, 0x083a00, 0x004000, 0x003c00, 0x00323c, 0x000000, 0x000000, 0x000000,
        0x989698, 0x084cc4, 0x3032ec, 0x5c1ee4, 0x8814b0, 0xa01464, 0x982220, 0x783c00,
        0x545a00, 0x287200, 0x087c00, 0x007628, 0x006678, 0x000000, 0x000000, 0x000000,
        0xeceeec, 0x4c9aec, 0x787cec, 0xb062ec, 0xe454ec, 0xec58b4, 0xec6a64, 0xd48820,
        0xa0aa00, 0x74c400, 0x4cd020, 0x38cc6c, 0x38b4cc, 0x3c3c3c, 0x000000, 0x000000,
        0xeceeec, 0xa8ccec, 0xbcbcec, 0xd4b2ec, 0xecaeec, 0xecaed4, 0xecb4b0, 0xe4c490,
        0xccd278, 0xb4de78, 0xa8e290, 0x98e2b4, 0xa0d6e4, 0xa0a2a0, 0x000000, 0x000000
    };
    uint32_t c = palette_rgb[nes_color & 0x3F];
    return rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static void render_scanline(ppu_t *ppu) {
    if (ppu->scanline >= 240) return;
    uint8_t bg_pixels[256];
    memset(bg_pixels, 0, sizeof(bg_pixels));

    if (ppu->reg_mask & 0x08) {
        uint16_t bg_base = (ppu->reg_ctrl & 0x10) ? 0x1000 : 0x0000;
        uint16_t v = ppu->v;
        uint8_t fine_x = ppu->x;

        for (int tile = 0; tile < 33; tile++) {
            uint16_t nt_addr = 0x2000 | (v & 0x0FFF);
            uint8_t tile_idx = ppu->vram[ppu_core_vram_mask(nt_addr, ppu->mirroring)];
            uint16_t fine_y = (v >> 12) & 0x07;
            uint16_t p_addr = bg_base + (tile_idx * 16) + fine_y;

            uint8_t p0 = ppu->vram[ppu_core_vram_mask(p_addr, ppu->mirroring)];
            uint8_t p1 = ppu->vram[ppu_core_vram_mask((uint16_t)(p_addr + 8), ppu->mirroring)];

            uint16_t at_addr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
            uint8_t attr = ppu->vram[ppu_core_vram_mask(at_addr, ppu->mirroring)];
            uint8_t pal_hi = (attr >> (((v >> 4) & 4) | (v & 2))) & 0x03;

            for (int dot = 0; dot < 8; dot++) {
                int px = (tile * 8) + dot - fine_x;
                if (px < 0 || px >= 256) continue;
                uint8_t color = ((p0 >> (7 - dot)) & 1) | (((p1 >> (7 - dot)) & 1) << 1);
                bg_pixels[px] = color;
                uint8_t pal_idx = (color == 0) ? 0 : (pal_hi << 2 | color);
                screen_buffer[ppu->scanline * 256 + px] = nes_color_to_1555(ppu->palette_ram[pal_idx]);
            }
            if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; } else v++;
        }
    } else {
        for(int px = 0; px < 256; px++) screen_buffer[ppu->scanline * 256 + px] = nes_color_to_1555(ppu->palette_ram[0]);
    }

    if (ppu->reg_mask & 0x10) {
        uint16_t spr_base = (ppu->reg_ctrl & 0x08) ? 0x1000 : 0x0000;
        int sprite_height = (ppu->reg_ctrl & 0x20) ? 16 : 8;
        int sprite_count = 0;
        int sprites_on_line[8];
        int sprite_indices[8];

        for (int i = 0; i < 64; i++) {
            int sy = ppu->oam[i * 4 + 0];
            int rel_y = ppu->scanline - (sy + 1);
            if (rel_y >= 0 && rel_y < sprite_height) {
                if (sprite_count < 8) {
                    sprites_on_line[sprite_count] = i * 4;
                    sprite_indices[sprite_count] = i;
                    sprite_count++;
                } else {
                    ppu->reg_status |= 0x20; 
                    break;
                }
            }
        }

        for (int s = sprite_count - 1; s >= 0; s--) {
            int oam_idx = sprites_on_line[s];
            int i = sprite_indices[s];
            int sy = ppu->oam[oam_idx + 0];
            uint8_t tile = ppu->oam[oam_idx + 1];
            uint8_t attr = ppu->oam[oam_idx + 2];
            int sx = ppu->oam[oam_idx + 3];
            int rel_y = ppu->scanline - (sy + 1);
            bool flip_y = (attr & 0x80) != 0;
            bool flip_x = (attr & 0x40) != 0;
            bool bg_priority = (attr & 0x20) != 0;
            uint8_t py = flip_y ? (sprite_height - 1 - rel_y) : rel_y;
            uint16_t p_addr;
            if (sprite_height == 16) {
                uint16_t table = (tile & 1) ? 0x1000 : 0x0000;
                uint8_t tile_idx = tile & 0xFE;
                if (py >= 8) { tile_idx++; py &= 7; }
                p_addr = table + (tile_idx * 16) + py;
            } else {
                p_addr = spr_base + (tile * 16) + py;
            }
            uint8_t p0 = ppu->vram[p_addr];
            uint8_t p1 = ppu->vram[p_addr + 8];
            for (int px = 0; px < 8; px++) {
                int draw_x = sx + px;
                if (draw_x >= 256) continue;
                uint8_t bit = flip_x ? px : (7 - px);
                uint8_t color = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
                if (color == 0) continue;
                if (i == 0 && bg_pixels[draw_x] != 0 && draw_x < 255) {
                    bool show_bg_left_8 = (ppu->reg_mask & 0x02) != 0;
                    bool show_spr_left_8 = (ppu->reg_mask & 0x04) != 0;
                    bool can_hit = true;
                    if (draw_x < 8 && (!show_bg_left_8 || !show_spr_left_8)) can_hit = false;
                    if (can_hit) ppu->reg_status |= 0x40;
                }
                if (bg_priority && bg_pixels[draw_x] != 0) continue;
                screen_buffer[ppu->scanline * 256 + draw_x] = nes_color_to_1555(ppu->palette_ram[0x10 + (attr & 0x03) * 4 + color]);
            }
        }
    }
}

void ppu_init(ppu_t *ppu) { 
    memset(ppu, 0, sizeof(*ppu)); 
    for (int i = 0; i < 32; i++) {
        ppu->palette_ram[i] = nintendo_power_up_palette[i];
    }
}

void ppu_reset(ppu_t *ppu) {
    ppu->scanline = 0; ppu->cycle = 0; ppu->v = 0; ppu->t = 0; ppu->w = 0;
    ppu->reg_status = 0; ppu->nmi_requested = 0; 
    ppu->open_bus = 0; ppu->open_bus_decay_timer = 0;
    ppu->vbl_suppress = false;
    ppu->frame = 0;
}

uint8_t ppu_cpu_read(ppu_t *ppu, uint16_t addr) {
    uint8_t res = ppu->open_bus; 
    switch (addr & 7) {
        case 2: // PPUSTATUS
            res = (ppu->reg_status & 0xE0) | (ppu->open_bus & 0x1F);

            if (ppu->scanline == 241 && ppu->cycle <= 3) {
                res |= 0x80;
                ppu->vbl_suppress = true;
                ppu->nmi_requested = 0;
            }
            if (ppu->scanline == 240 && ppu->cycle >= 339) {
                ppu->vbl_suppress = true;
            }

            ppu->reg_status &= ~0x80; 
            ppu->w = 0;
            break;

        case 4: // OAMDATA
            res = ppu->oam[ppu->oam_addr];
            if ((ppu->oam_addr & 0x03) == 0x02) res &= 0xE3;
            break;

        case 7: { // $2007 - PPUDATA
            uint16_t v_addr = ppu->v & 0x3FFF;
            uint8_t current_buffer = ppu->vram_read_buffer;

            if (v_addr < 0x3F00) {
                ppu->vram_read_buffer = ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)];
                res = current_buffer;
            } else {
                // Palette okuma mantığı
                ppu->vram_read_buffer = ppu->vram[ppu_core_vram_mask(v_addr - 0x1000, ppu->mirroring)];
                res = ppu->palette_ram[ppu_core_palette_mirror_index(v_addr & 0x1F)];

                // Palette RAM Quirks: Greyscale mode aktifse alt 4 bit sıfır olmalı
                if (ppu->reg_mask & 0x01) {
                    res &= 0x30;
                }
                
                // Open bus bitleri (6-7) palette okumasında korunur
                res = (res & 0x3F) | (ppu->open_bus & 0xC0);
            }

            ppu->v = (ppu->v + ((ppu->reg_ctrl & 0x04) ? 32 : 1)) & 0x7FFF;
            break;
        }
    }
    
    ppu->open_bus = res;
    ppu->open_bus_decay_timer = 0;
    return res;
}

void ppu_cpu_write(ppu_t *ppu, uint16_t addr, uint8_t val) {
    ppu->open_bus = val;
    ppu->open_bus_decay_timer = 0;
    switch (addr & 7) {
        case 0: {
            bool old_nmi = (ppu->reg_ctrl & 0x80) != 0;
            ppu->reg_ctrl = val; 
            ppu->t = (ppu->t & ~0x0C00) | ((val & 0x03) << 10); 
            if (!old_nmi && (val & 0x80) && (ppu->reg_status & 0x80))
                emu_driver_request_nmi_delayed();
        } break;
        case 1: ppu->reg_mask = val; break;
        case 3: ppu->oam_addr = val; break;
        case 4: ppu->oam[ppu->oam_addr++] = val; break;
        case 5:
            if (ppu->w == 0) { ppu->t = (ppu->t & ~0x001F) | (val >> 3); ppu->x = val & 0x07; ppu->w = 1; }
            else { ppu->t = (ppu->t & ~0x73E0) | ((val & 0x07) << 12) | ((val & 0xF8) << 2); ppu->w = 0; }
            break;
        case 6:
            if (ppu->w == 0) { ppu->t = (ppu->t & 0x00FF) | ((val & 0x3F) << 8); ppu->w = 1; }
            else { ppu->t = (ppu->t & 0xFF00) | val; ppu->v = ppu->t; ppu->w = 0; }
            break;
        case 7: {
            uint16_t v_addr = ppu->v & 0x3FFF;
            if (v_addr >= 0x3F00) {
                ppu->palette_ram[ppu_core_palette_mirror_index(v_addr & 0x1F)] = val;
            } else if (v_addr < 0x2000) {
                // CHR ROM is not Writable kontrolü
                if (!nes_chr_is_rom) {
                    ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)] = val;
                }
            } else {
                ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)] = val;
            }
            ppu->v = (ppu->v + ((ppu->reg_ctrl & 0x04) ? 32 : 1)) & 0x7FFF;
        } break;
    }
}

void ppu_step_cycles(ppu_t *ppu, uint32_t cycles) {
    for (uint32_t i = 0; i < cycles; i++) {
        ppu->open_bus_decay_timer++;
        if (ppu->open_bus_decay_timer > 5369319) {
            ppu->open_bus = 0x00;
            ppu->open_bus_decay_timer = 0;
        }

        bool rendering = (ppu->reg_mask & 0x18) != 0;

        if (ppu->scanline == 261 && ppu->cycle == 339 && rendering && (ppu->frame % 2 != 0)) {
            ppu->cycle = 0; ppu->scanline = 0; ppu->frame++; ppu->vbl_suppress = false;
            ppu->reg_status &= ~0xE0; continue; 
        }

        if (ppu->cycle == 256 && rendering && ppu->scanline < 240) render_scanline(ppu);

        if (rendering && (ppu->scanline < 240 || ppu->scanline == 261)) {
            if (ppu->cycle == 256) {
                if ((ppu->v & 0x7000) != 0x7000) ppu->v += 0x1000;
                else {
                    ppu->v &= ~0x7000;
                    int y = (ppu->v & 0x03E0) >> 5;
                    if (y == 29) { y = 0; ppu->v ^= 0x0800; }
                    else if (y == 31) y = 0;
                    else y++;
                    ppu->v = (ppu->v & ~0x03E0) | (y << 5);
                }
            }
            if (ppu->cycle == 257) ppu->v = (ppu->v & ~0x041F) | (ppu->t & 0x041F);
            if (ppu->scanline == 261 && ppu->cycle >= 280 && ppu->cycle <= 304) ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
        }

        ppu->cycle++;
        if (ppu->cycle >= 341) {
            ppu->cycle = 0;
            ppu->scanline++;

            if (ppu->scanline == 261) {
                ppu->reg_status &= ~0xE0; ppu->nmi_requested = 0; ppu->vbl_suppress = false;
            } else if (ppu->scanline >= 262) {
                ppu->scanline = 0; ppu->frame++; ppu->vbl_suppress = false;
            }
        }

        if (ppu->scanline == 241 && ppu->cycle == 2) {
            if (!ppu->vbl_suppress) {
                ppu->reg_status |= 0x80;
                if (ppu->reg_ctrl & 0x80) ppu->nmi_requested = 1;
            }
        }
    }
}

void ppu_draw(const ppu_t *ppu) {
    int x_off = video_width() - 256 - 20, y_off = 40;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 256; x++) video_draw_pixel(x_off + x, y_off + y, screen_buffer[y * 256 + x]);
    }
}

void ppu_oam_dma(ppu_t *ppu, const uint8_t page[256]) { memcpy(ppu->oam, page, 256); }