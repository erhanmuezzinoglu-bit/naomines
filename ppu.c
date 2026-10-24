/*
 * ppu.c - NES PPU (2C02) emülasyonu
 *
 * Genel yapı
 *  - ppu_step_cycles() PPU'yu dot dot ilerletir; CPU her bus erişiminden önce
 *    3 dot çağırır.
 *  - Normal görünür satırlar hızlı yoldan (render_scanline, dot 256'da) çizilir.
 *    $2001 satır ortasında değişirse ya da bayat shifter durumu varsa o satır
 *    render_line_dotwise() ile dot dot simüle edilir. İki yol düzenli satırlarda
 *    piksel piksel aynı çıktıyı verir.
 *  - Sprite pipeline (OAM2 temizleme, değerlendirme, fetch, OAMADDR sıfırlama,
 *    $2004 veri yolu) "sanal dot" üzerinde çalışır; aşağıya bakın.
 */

#include "ppu.h"
#include "nes_rom.h"
#include "exp_config.h"
#include <naomi/video.h>
#include <string.h>
#include <stdbool.h>

/* Mapper kancaları (mapper.c). */
extern void mapper_scanline_clock(void);

/* ============================================================
 *  Sanal dot
 * ============================================================ */
/* (yorum bloğu aynı) */

#ifndef PPU_SPRITE_PIPELINE_LEAD
#define PPU_SPRITE_PIPELINE_LEAD 3
#endif

#ifndef PPU_SPR0_HIT_DELAY
#define PPU_SPR0_HIT_DELAY 0
#endif

#ifndef PPU_OVERFLOW_SET_DELAY
#define PPU_OVERFLOW_SET_DELAY 0
#endif

#ifndef PPU_SPRITE_FLAG_CLEAR_LEAD
#define PPU_SPRITE_FLAG_CLEAR_LEAD 2
#endif

#ifndef PPU_MASK_ENABLE_DELAY
#define PPU_MASK_ENABLE_DELAY 4
#endif

#ifndef PPU_BG_FILL_DOTS
#define PPU_BG_FILL_DOTS 16
#endif

#ifndef PPU_MASK_HIT_DELAY
#define PPU_MASK_HIT_DELAY 4
#endif

#ifndef PPU_MASK_WRITE_DELAY
#define PPU_MASK_WRITE_DELAY 3
#endif

#ifndef PPU_2007_BUFFER_DELAY
#define PPU_2007_BUFFER_DELAY 3
#endif

#ifndef PPU_EMULATE_OAM_CORRUPTION
#define PPU_EMULATE_OAM_CORRUPTION 1
#endif

#ifndef PPU_BLOCK_OAM_WRITE_WHILE_RENDERING
#define PPU_BLOCK_OAM_WRITE_WHILE_RENDERING 1
#endif

#ifndef PPU_DOTWISE_ALWAYS
#define PPU_DOTWISE_ALWAYS 0
#endif

/* ============================================================
 *  Dahili sabitler
 * ============================================================ */

enum {
    BGEV_HYBRID    = 1,
    BGEV_AFTER_NT  = 2,
    BGEV_LATCH_LOW = 3
};

enum {
    EVAL_RANGE    = 0,
    EVAL_COPY     = 1,
    EVAL_OVERFLOW = 2,
    EVAL_DONE     = 3
};

static void render_scanline(ppu_t *ppu);

#if EXP_FAST_BLIT
/* Donanim piksel formati (RGB0555, 16 bit). Satir basi 256 piksel. */
typedef uint16_t scr_pixel_t;
#else
typedef color_t scr_pixel_t;
#endif
static scr_pixel_t screen_buffer[256 * 240] __attribute__((aligned(32)));

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
        switch (mirroring) {
            case 0:
                return 0x2000 | (addr & 0x03FF) | ((addr & 0x0800) >> 1);
            case 1:
                return 0x2000 | (addr & 0x07FF);
            case 2:
                return 0x2000 | (addr & 0x03FF);
            case 3:
                return 0x2400 | (addr & 0x03FF);
            default:
                return 0x2000 | (addr & 0x07FF);
        }
    }
    return a;
}

uint8_t ppu_core_palette_mirror_index(uint8_t idx) {
    idx &= 0x1F;
    if ((idx & 0x03) == 0x00 && idx >= 0x10) idx &= 0x0F;
    return idx;
}

static const uint32_t nes_palette_rgb[64] = {
    0x545454, 0x001e74, 0x081090, 0x300088, 0x440064, 0x5c0030, 0x540400, 0x3c1800,
    0x202a00, 0x083a00, 0x004000, 0x003c00, 0x00323c, 0x000000, 0x000000, 0x000000,
    0x989698, 0x084cc4, 0x3032ec, 0x5c1ee4, 0x8814b0, 0xa01464, 0x982220, 0x783c00,
    0x545a00, 0x287200, 0x087c00, 0x007628, 0x006678, 0x000000, 0x000000, 0x000000,
    0xeceeec, 0x4c9aec, 0x787cec, 0xb062ec, 0xe454ec, 0xec58b4, 0xec6a64, 0xd48820,
    0xa0aa00, 0x74c400, 0x4cd020, 0x38cc6c, 0x38b4cc, 0x3c3c3c, 0x000000, 0x000000,
    0xeceeec, 0xa8ccec, 0xbcbcec, 0xd4b2ec, 0xecaeec, 0xecaed4, 0xecb4b0, 0xe4c490,
    0xccd278, 0xb4de78, 0xa8e290, 0x98e2b4, 0xa0d6e4, 0xa0a2a0, 0x000000, 0x000000
    };

#if EXP_FAST_BLIT

static uint16_t s_pal16[64];
/* 0: ppu_init cagrilmadi, 1: hizli blit, 2: yedek (yavas) yol: dikey monitor
 * veya framebuffer yerlesimi beklenenle uyusmadi. */
static int s_blit_mode = 0;

static inline uint16_t rgb555_fallback(uint32_t c) {
    return (uint16_t)((((c >> 19) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F));
}

static inline scr_pixel_t nes_color_to_1555(uint8_t nes_color) {
    return s_pal16[nes_color & 0x3F];
}

/* video_init()'ten SONRA cagrilmali. Donanim piksel formatini makroya bakmadan,
 * ayni rengi video_draw_pixel ile cizip ham 16 biti geri okuyarak turetir ve
 * framebuffer'in satir-major yerlesimini kucuk bir testle dogrular. */
static void ppu_blit_init(void)
{
    for (int i = 0; i < 64; i++) s_pal16[i] = rgb555_fallback(nes_palette_rgb[i]);
    s_blit_mode = 2;

    if (video_is_vertical() || video_depth() != 2) return;

    volatile uint16_t *fb = (volatile uint16_t *)video_framebuffer();
    const unsigned w = video_width();

    uint16_t save0 = fb[0];
    for (int i = 0; i < 64; i++) {
        uint32_t c = nes_palette_rgb[i];
        video_draw_pixel(0, 0, rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
        s_pal16[i] = fb[0];
    }
    fb[0] = save0;

    /* Yerlesim testi: (x=5, y=3) -> fb[3*w + 5] olmali. */
    const unsigned idx = 3u * w + 5u;
    uint16_t save = fb[idx];
    uint32_t c = nes_palette_rgb[0x21];
    video_draw_pixel(5, 3, rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
    int ok = (fb[idx] == s_pal16[0x21]);
    fb[idx] = save;

    if (ok) s_blit_mode = 1;
}

int ppu_blit_mode(void) { return s_blit_mode; }

#else /* !EXP_FAST_BLIT: orijinal davranis */

static inline scr_pixel_t nes_color_to_1555(uint8_t nes_color) {
    uint32_t c = nes_palette_rgb[nes_color & 0x3F];
    return rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

int ppu_blit_mode(void) { return 0; }

#endif

static void ppu_a12_check(uint16_t old_v, uint16_t new_v)
{
    if ((old_v ^ new_v) & 0x1000) {
        if (new_v & 0x1000)
            mapper_scanline_clock();
    }
}

static inline bool ppu_rendering_line(const ppu_t *ppu)
{
    return (ppu->reg_mask & 0x18) &&
           (ppu->scanline < 240 || ppu->scanline == 261);
}

static inline uint8_t rev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

static inline void spr_virtual_dot(int real_sl, int real_cycle, int *vsl, int *vdot)
{
    int d = real_cycle + PPU_SPRITE_PIPELINE_LEAD;
    int sl = real_sl;
    if (d >= 341) { d -= 341; sl++; if (sl >= 262) sl = 0; }
    *vsl = sl;
    *vdot = d;
}

static inline uint8_t oam_read_masked(const ppu_t *ppu, uint8_t addr)
{
    uint8_t v = ppu->oam[addr];
    if ((addr & 3) == 2) v &= 0xE3;
    return v;
}

static inline int spr_in_range(int scanline, uint8_t y, int height)
{
    int rel = scanline - (int)y;
    return rel >= 0 && rel < height;
}

static inline void v_inc_coarse_x(uint16_t *v)
{
    if ((*v & 0x001F) == 31) { *v &= ~0x001F; *v ^= 0x0400; }
    else (*v)++;
}

static inline void v_inc_y(uint16_t *v)
{
    if ((*v & 0x7000) != 0x7000) {
        *v += 0x1000;
    } else {
        *v &= ~0x7000;
        int y = (*v & 0x03E0) >> 5;
        if (y == 29)      { y = 0; *v ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else              { y++; }
        *v = (uint16_t)((*v & ~0x03E0) | (y << 5));
    }
}

static void ppu_advance_v_after_data_access(ppu_t *ppu)
{
    if (ppu_rendering_line(ppu)) {
        v_inc_coarse_x(&ppu->v);
        v_inc_y(&ppu->v);
    } else {
        ppu->v = (ppu->v + ((ppu->reg_ctrl & 0x04) ? 32 : 1)) & 0x7FFF;
    }
}

/* ============================================================
 *  Sprite pipeline
 * ============================================================ */

static void sprite_clear_dot(ppu_t *ppu, int dot)
{
    if (dot == 1) ppu->eval_found = 0;
    ppu->oam_buffer = 0xFF;
    ppu->sec_oam[(dot - 1) >> 1] = 0xFF;
}

static inline void sprite_oam2_inc(ppu_t *ppu)
{
    if (++ppu->oam2_addr >= 32) {
        ppu->oam2_addr = 0;
        ppu->oam2_frozen = 1;
    }
}

static void sprite_eval_dot(ppu_t *ppu, int sl, int dot)
{
    if (dot == 65) {
        ppu->eval_phase     = EVAL_RANGE;
        ppu->eval_ptr       = 0;
        ppu->eval_copy_left = 0;
        ppu->eval_ovf_left  = 0;
        ppu->spr0_sec       = 0;
    }

    if (dot & 1) {
        if (ppu->eval_phase == EVAL_OVERFLOW && ppu->eval_ovf_left == 1) {
            ppu->oam_buffer = ppu->sec_oam[ppu->eval_ptr & 31];
        } else {
            ppu->oam_buffer = oam_read_masked(ppu, ppu->oam_addr);
        }
        return;
    }

    uint8_t s = ppu->oam_buffer;
    int height = (ppu->reg_ctrl & 0x20) ? 16 : 8;
    int in_range = spr_in_range(sl, s, height);
    int addr = ppu->oam_addr;
    int next;
    int mask_fc = 0;

    switch (ppu->eval_phase) {
    case EVAL_RANGE:
        if (ppu->eval_ptr < 32) ppu->sec_oam[ppu->eval_ptr] = s;
        if (dot == 66 && in_range) ppu->spr0_sec = 1;

        if (in_range) {
            ppu->eval_ptr++;
            if (ppu->eval_found < 8) ppu->eval_found++;
            if (ppu->eval_found >= 8) ppu->oam2_frozen = 1;
            ppu->eval_copy_left = 3;
            ppu->eval_phase = EVAL_COPY;
            next = addr + 1;
        } else {
            next = addr + 4;
            mask_fc = 1;
        }
        break;

    case EVAL_COPY:
        if (ppu->eval_ptr < 32) ppu->sec_oam[ppu->eval_ptr] = s;
        ppu->eval_ptr++;
        ppu->eval_copy_left--;
        next = addr + 1;
        if (ppu->eval_copy_left == 0) {
            mask_fc = in_range ? 0 : 1;
            if (ppu->eval_ptr >= 32) {
                ppu->eval_phase = EVAL_OVERFLOW;
                ppu->oam2_frozen = 1;
            } else {
                ppu->eval_phase = EVAL_RANGE;
            }
        }
        break;

    case EVAL_OVERFLOW:
        if (ppu->eval_ovf_left) {
            ppu->eval_ovf_left--;
            next = addr + 1;
            if (ppu->eval_ovf_left == 0) ppu->eval_phase = EVAL_DONE;
        } else if (in_range) {
            ppu->ovf_set_cycle = dot - PPU_SPRITE_PIPELINE_LEAD + 1 + PPU_OVERFLOW_SET_DELAY;
            ppu->eval_ovf_left = 4;
            next = addr + 1;
        } else {
            int n = (addr >> 2) + 1;
            int m = ((addr & 3) + 1) & 3;
            if (n > 63) { ppu->eval_phase = EVAL_DONE; n &= 63; }
            next = (n << 2) | m;
        }
        ppu->oam_buffer = ppu->sec_oam[ppu->eval_ptr & 31];
        break;

    default:
        ppu->oam_addr = (uint8_t)(addr + 4);
        ppu->oam_buffer = ppu->sec_oam[ppu->eval_ptr & 31];
        return;
    }

    if (next > 0xFF) ppu->eval_phase = EVAL_DONE;
    next &= 0xFF;
    if (mask_fc) next &= 0xFC;
    ppu->oam_addr = (uint8_t)next;
}

static void sprite_load_slot(ppu_t *ppu, int sl, int k)
{
    uint8_t y = ppu->fetch_byte[0], tile = ppu->fetch_byte[1];
    uint8_t attr = ppu->fetch_byte[2], x = ppu->fetch_byte[3];
    int height = (ppu->reg_ctrl & 0x20) ? 16 : 8;
    int rel;

    if (sl == 261) rel = (261 - (int)y) & 0xFF;
    else           rel = sl - (int)y;

    int valid = rel >= 0 && rel < height;
    if (sl == 261 && k >= ppu->eval_found) valid = 0;

    if (k == 0) ppu->spr0_line = (uint8_t)(ppu->spr0_sec && valid);

    if (!valid) {
        ppu->spr_valid[k] = 0;
        ppu->sps.lo[k] = 0; ppu->sps.hi[k] = 0;
        ppu->sps.attr[k] = attr; ppu->sps.cnt[k] = x;
        return;
    }

    int row = (attr & 0x80) ? (height - 1 - rel) : rel;
    uint16_t addr;

    if (height == 16) {
        uint16_t table = (tile & 1) ? 0x1000 : 0x0000;
        uint8_t t = tile & 0xFE;
        if (row >= 8) { t++; row &= 7; }
        addr = (uint16_t)(table + t * 16 + row);
    } else {
        uint16_t base = (ppu->reg_ctrl & 0x08) ? 0x1000 : 0x0000;
        addr = (uint16_t)(base + tile * 16 + row);
    }

    ppu->spr_valid[k] = 1;
    ppu->spr_x[k]     = x;
    ppu->spr_attr[k]  = attr;
    ppu->spr_lo[k]    = ppu->vram[addr & 0x1FFF];
    ppu->spr_hi[k]    = ppu->vram[(addr + 8) & 0x1FFF];

    {
        uint8_t l = ppu->spr_lo[k], h = ppu->spr_hi[k];
        if (attr & 0x40) { l = rev8(l); h = rev8(h); }
        ppu->sps.lo[k] = l; ppu->sps.hi[k] = h;
        ppu->sps.attr[k] = attr; ppu->sps.cnt[k] = x;
    }
}

static void sprite_fetch_dot(ppu_t *ppu, int sl, int c)
{
    int off = (c - 257) & 7;
    int slot = (c - 257) >> 3;

    if (c == 257) ppu->oam2_addr = 0;

    if (ppu->oam2_frozen) {
        uint8_t b0 = ppu->sec_oam[0];
        ppu->fetch_byte[0] = ppu->fetch_byte[1] = ppu->fetch_byte[2] = ppu->fetch_byte[3] = b0;
    }

    if (off < 4) {
        uint8_t b = ppu->sec_oam[ppu->oam2_frozen ? 0 : (ppu->oam2_addr & 31)];
        ppu->fetch_byte[off] = b;
        ppu->oam_buffer = b;
        if (off < 3 && !ppu->oam2_frozen) sprite_oam2_inc(ppu);
    } else {
        ppu->oam_buffer = ppu->fetch_byte[3];
    }

    if (off == 7) {
        if (!ppu->oam2_frozen) sprite_oam2_inc(ppu);
        sprite_load_slot(ppu, sl, slot);
    }
}

#if PPU_EMULATE_OAM_CORRUPTION
static int oam_corrupt_seed(const ppu_t *ppu, int c)
{
    if (c >= 1 && c <= 64)    return (c - 1) >> 1;
    if (c >= 65 && c <= 256)  return ((ppu->eval_ptr + 3) & ~3) & 31;
    if (c >= 257 && c <= 320) return ppu->oam2_addr & 31;
    return 0;
}

static void oam_corrupt_apply(ppu_t *ppu)
{
    int row = ppu->oam_corrupt_row & 31;
    if (row) {
        memcpy(&ppu->oam[row * 8], &ppu->oam[0], 8);
        ppu->sec_oam[row] = ppu->sec_oam[0];
    }
}
#endif

/* ============================================================
 *  Arka plan satırı
 * ============================================================ */
static void render_bg_row(ppu_t *ppu, uint8_t bg_pixels[256], int draw, int force_bg)
{
    memset(bg_pixels, 0, 256);

    if (!force_bg && !(ppu->reg_mask & 0x08)) {
        if (draw) {
            for (int px = 0; px < 256; px++)
                screen_buffer[ppu->scanline * 256 + px] = nes_color_to_1555(ppu->palette_ram[0]);
        }
        return;
    }

    uint16_t v       = ppu->line_latched ? ppu->line_v : ppu->v;
    uint8_t  fine_x  = ppu->line_latched ? ppu->line_x : ppu->x;
    uint16_t bg_base = (ppu->line_latched ? ppu->line_bg_hi : ((ppu->reg_ctrl & 0x10) != 0))
                       ? 0x1000 : 0x0000;

    for (int tile = 0; tile < 33; tile++) {
        int ev_hybrid = 0, ev_after = 0, ev_latch = 0;
        uint8_t ev_nt = 0;
        uint16_t ev_v = 0;
        for (int i = 0; i < ppu->bg_ev_count; i++) {
            if (ppu->bg_ev_tile[i] != tile) continue;
            switch (ppu->bg_ev_kind[i]) {
                case BGEV_HYBRID:    ev_hybrid = 1; ev_nt = ppu->bg_ev_val[i]; ev_v = ppu->bg_ev_v[i]; break;
                case BGEV_AFTER_NT:  ev_after  = 1; ev_v = ppu->bg_ev_v[i]; break;
                case BGEV_LATCH_LOW: ev_latch  = 1; break;
                default: break;
            }
        }

        uint16_t nt_addr = 0x2000 | (v & 0x0FFF);
        uint8_t tile_idx = ev_hybrid ? ev_nt
                                     : ppu->vram[ppu_core_vram_mask(nt_addr, ppu->mirroring)];
        if (ev_hybrid || ev_after) v = ev_v;

        uint16_t fine_y = (v >> 12) & 0x07;
        uint16_t p_addr = bg_base + (tile_idx * 16) + fine_y;

        uint16_t at_addr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
        uint8_t attr = ppu->vram[ppu_core_vram_mask(at_addr, ppu->mirroring)];
        uint8_t pal_hi = (attr >> (((v >> 4) & 4) | (v & 2))) & 0x03;

        uint8_t p0 = ppu->vram[ppu_core_vram_mask(p_addr, ppu->mirroring)];
        if (ev_latch) {
            uint16_t la = (uint16_t)((p_addr & 0xFF00) | attr);
            p0 = ppu->vram[ppu_core_vram_mask(la, ppu->mirroring)];
        }
        uint8_t p1 = ppu->vram[ppu_core_vram_mask((uint16_t)(p_addr + 8), ppu->mirroring)];

        for (int dot = 0; dot < 8; dot++) {
            int px = (tile * 8) + dot - fine_x;
            if (px < 0 || px >= 256) continue;
            uint8_t color = ((p0 >> (7 - dot)) & 1) | (((p1 >> (7 - dot)) & 1) << 1);
            bg_pixels[px] = color;
            if (draw) {
                uint8_t pal_idx = (color == 0) ? 0 : (pal_hi << 2 | color);
                screen_buffer[ppu->scanline * 256 + px] =
                    nes_color_to_1555(ppu->palette_ram[pal_idx]);
            }
        }
        v_inc_coarse_x(&v);
    }
}

/* ============================================================
 *  Sprite 0 hit zamanlama
 * ============================================================ */
static void sprite0_schedule_hit(ppu_t *ppu, int dot_now)
{
    ppu->spr0_hit_cycle = 0;

    if (ppu->scanline >= 240) return;
    if (!ppu->spr0_line || !ppu->spr_valid[0]) return;
    if ((ppu->reg_mask & 0x18) != 0x18) return;
    if (ppu->reg_status & 0x40) return;

    int age_now = ppu->render_age;
    uint8_t bg[256];
    render_bg_row(ppu, bg, 0, 1);

    uint8_t attr = ppu->spr_attr[0];
    int sx = ppu->spr_x[0];
    bool flip_x = (attr & 0x40) != 0;
    bool clip = !(ppu->reg_mask & 0x02) || !(ppu->reg_mask & 0x04);

    for (int px = 0; px < 8; px++) {
        int x = sx + px;
        if (x >= 255) break;
        uint8_t bit = flip_x ? px : (7 - px);
        uint8_t color = ((ppu->spr_lo[0] >> bit) & 1) | (((ppu->spr_hi[0] >> bit) & 1) << 1);
        if (color == 0 || bg[x] == 0) continue;
        if (x < 8 && clip) continue;

        int d = x + 1;
        if (dot_now > 1) {
            if (d < dot_now + PPU_MASK_HIT_DELAY) continue;
            if (age_now + (d - dot_now) < PPU_BG_FILL_DOTS) continue;
        }

        int hc = x + PPU_SPR0_HIT_DELAY;
        if (hc < 2) hc = 2;
        if (hc <= dot_now) hc = dot_now + 1;
        ppu->spr0_hit_cycle = hc;
        return;
    }
}

/* ============================================================
 *  BG fetch sırasında $2006 / $2007 adres yolu etkileri
 * ============================================================ */
static void bg_add_event(ppu_t *ppu, int kind, int tile, uint8_t val, uint16_t v)
{
    if (ppu->bg_ev_count >= 4) return;
    int i = ppu->bg_ev_count++;
    ppu->bg_ev_kind[i] = (uint8_t)kind;
    ppu->bg_ev_tile[i] = (uint8_t)tile;
    ppu->bg_ev_val[i]  = val;
    ppu->bg_ev_v[i]    = v;
}

static uint16_t bg_v_at_tile(const ppu_t *ppu, int k)
{
    uint16_t v = ppu->line_v;
    for (int t = 0; t < k; t++) {
        for (int i = 0; i < ppu->bg_ev_count; i++) {
            if (ppu->bg_ev_tile[i] == t &&
                (ppu->bg_ev_kind[i] == BGEV_HYBRID || ppu->bg_ev_kind[i] == BGEV_AFTER_NT))
                v = ppu->bg_ev_v[i];
        }
        v_inc_coarse_x(&v);
    }
    return v;
}

static void bg_note_v_write(ppu_t *ppu, uint16_t v_new)
{
    if (!(ppu->reg_mask & 0x18) || !ppu->line_latched) return;
    if (ppu->scanline >= 240) return;

    int vsl, vd;
    spr_virtual_dot(ppu->scanline, ppu->cycle, &vsl, &vd);
    if (vsl != ppu->scanline || vd < 4 || vd > 250) return;

    int tf = (vd - 1) >> 3;
    int p  = (vd - 1) & 7;
    int k  = tf + 2;
    if (k > 32) return;

    if (p <= 2) {
        uint16_t v_t = bg_v_at_tile(ppu, k);
        uint16_t nt_old = 0x2000 | (v_t & 0x0FFF);
        uint16_t hi_new = (uint16_t)((0x2000 | (v_new & 0x0FFF)) & 0xFF00);
        uint16_t addr = (uint16_t)(hi_new | (nt_old & 0x00FF));
        uint8_t val = ppu->vram[ppu_core_vram_mask(addr, ppu->mirroring)];
        bg_add_event(ppu, BGEV_HYBRID, k, val, v_new);
    } else {
        bg_add_event(ppu, BGEV_AFTER_NT, k, 0, v_new);
    }
    sprite0_schedule_hit(ppu, ppu->cycle);
}

static void bg_note_2007_read(ppu_t *ppu)
{
    if (!(ppu->reg_mask & 0x18) || !ppu->line_latched) return;
    if (ppu->scanline >= 240) return;

    int vsl, vd;
    spr_virtual_dot(ppu->scanline, ppu->cycle, &vsl, &vd);
    if (vsl != ppu->scanline || vd < 4 || vd > 240) return;
    if (((vd + 1) & 7) > 2) return;

    int base = (vd + 1) & ~7;
    int tf = (base + 5 - 1) >> 3;
    int k = tf + 2;
    if (k > 32) return;

    bg_add_event(ppu, BGEV_LATCH_LOW, k, 0, 0);
    sprite0_schedule_hit(ppu, ppu->cycle);
}

/* ============================================================
 *  Dot bazlı BG / sprite shifter simülasyonu
 * ============================================================ */
static inline uint8_t bgx_nt(const ppu_t *p, uint16_t v)
{
    return p->vram[ppu_core_vram_mask((uint16_t)(0x2000 | (v & 0x0FFF)), p->mirroring)];
}

static inline void bgx_at(const ppu_t *p, uint16_t v, uint8_t *pal, uint8_t *raw)
{
    uint16_t a = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
    uint8_t b = p->vram[ppu_core_vram_mask(a, p->mirroring)];
    *raw = b;
    *pal = (uint8_t)((b >> (((v >> 4) & 4) | (v & 2))) & 3);
}

static inline uint8_t bgx_pat(const ppu_t *p, uint16_t base, uint8_t nt, uint16_t v, int hi)
{
    uint16_t a = (uint16_t)(base + nt * 16 + ((v >> 12) & 7) + (hi ? 8 : 0));
    return p->vram[ppu_core_vram_mask(a, p->mirroring)];
}

static inline void bg_reload(ppu_bgstate_t *bs)
{
    bs->lo  = (uint16_t)((bs->lo  & 0xFF00) | bs->plo);
    bs->hi  = (uint16_t)((bs->hi  & 0xFF00) | bs->phi);
    bs->alo = (uint16_t)((bs->alo & 0xFF00) | ((bs->pal & 1) ? 0xFF : 0));
    bs->ahi = (uint16_t)((bs->ahi & 0xFF00) | ((bs->pal & 2) ? 0xFF : 0));
}

static inline void bg_shift(ppu_bgstate_t *bs)
{
    bs->lo  = (uint16_t)(bs->lo << 1);
    bs->hi  = (uint16_t)((bs->hi << 1) | 1);
    bs->alo = (uint16_t)((bs->alo << 1) | (bs->pal & 1));
    bs->ahi = (uint16_t)((bs->ahi << 1) | ((bs->pal >> 1) & 1));
}

static void bg_standard_start(const ppu_t *ppu, ppu_bgstate_t *bs)
{
    uint16_t v = ppu->line_latched ? ppu->line_v : ppu->v;
    uint16_t base = (ppu->line_latched ? ppu->line_bg_hi : ((ppu->reg_ctrl & 0x10) != 0)) ? 0x1000 : 0x0000;
    uint8_t lo[2], hi[2], pal[2], nt = 0, raw = 0;
    for (int i = 0; i < 2; i++) {
        nt = bgx_nt(ppu, v);
        bgx_at(ppu, v, &pal[i], &raw);
        lo[i] = bgx_pat(ppu, base, nt, v, 0);
        hi[i] = bgx_pat(ppu, base, nt, v, 1);
        v_inc_coarse_x(&v);
    }
    bs->lo  = (uint16_t)((lo[0] << 8) | lo[1]);
    bs->hi  = (uint16_t)((hi[0] << 8) | hi[1]);
    bs->alo = (uint16_t)(((pal[0] & 1) ? 0xFF00 : 0) | ((pal[1] & 1) ? 0x00FF : 0));
    bs->ahi = (uint16_t)(((pal[0] & 2) ? 0xFF00 : 0) | ((pal[1] & 2) ? 0x00FF : 0));
    bs->nt = nt; bs->pal = pal[1]; bs->praw = raw; bs->plo = lo[1]; bs->phi = hi[1];
    bs->v = v;
}

static void render_line_dotwise(ppu_t *ppu, int draw, int *hit_x)
{
    if (hit_x) *hit_x = -1;

    ppu_bgstate_t bs;
    if (ppu->bgs_start_valid) bs = ppu->bgs;
    else                      bg_standard_start(ppu, &bs);
    ppu_spstate_t ss = ppu->sps;

    uint8_t m = ppu->rt_mask0;
    int ei = 0;
    int fx = ppu->line_latched ? ppu->line_x : ppu->x;
    uint16_t base = (ppu->line_latched ? ppu->line_bg_hi : ((ppu->reg_ctrl & 0x10) != 0)) ? 0x1000 : 0x0000;
    int ev_hyb = 0, ev_aft = 0, ev_lat = 0;
    uint8_t ev_nt = 0;
    uint16_t ev_v = 0;
    int hit = -1;

    for (int d = 1; d <= 257; d++) {
        while (ei < ppu->rt_count && ppu->rt_dot[ei] <= d) m = ppu->rt_mask[ei++];
        int on = (m & 0x18) != 0;
        int x = d - 1;

        if (!on) {
            if (draw && d <= 256)
                screen_buffer[ppu->scanline * 256 + x] = nes_color_to_1555(ppu->palette_ram[0]);
            if (d <= 256) {
                for (int k = 0; k < 8; k++) {
                    if (ss.halted[k]) continue;
                    if (ss.cnt[k] == 0) ss.halted[k] = 1;
                    else ss.cnt[k]--;
                }
            }
            continue;
        }

        if (d >= 2) bg_shift(&bs);
        if (d >= 9 && ((d - 1) & 7) == 0) bg_reload(&bs);

        if (d <= 256) {
            int ph = (d - 1) & 7;
            int k = ((d - 1) >> 3) + 2;
            if (ph == 1) {
                ev_hyb = ev_aft = ev_lat = 0; ev_nt = 0; ev_v = 0;
                for (int i = 0; i < ppu->bg_ev_count; i++) {
                    if (ppu->bg_ev_tile[i] != k) continue;
                    switch (ppu->bg_ev_kind[i]) {
                        case BGEV_HYBRID:    ev_hyb = 1; ev_nt = ppu->bg_ev_val[i]; ev_v = ppu->bg_ev_v[i]; break;
                        case BGEV_AFTER_NT:  ev_aft = 1; ev_v = ppu->bg_ev_v[i]; break;
                        case BGEV_LATCH_LOW: ev_lat = 1; break;
                        default: break;
                    }
                }
                bs.nt = ev_hyb ? ev_nt : bgx_nt(ppu, bs.v);
                if (ev_hyb || ev_aft) bs.v = ev_v;
            } else if (ph == 3) {
                bgx_at(ppu, bs.v, &bs.pal, &bs.praw);
            } else if (ph == 5) {
                bs.plo = bgx_pat(ppu, base, bs.nt, bs.v, 0);
                if (ev_lat) {
                    uint16_t pa = (uint16_t)(base + bs.nt * 16 + ((bs.v >> 12) & 7));
                    bs.plo = ppu->vram[ppu_core_vram_mask((uint16_t)((pa & 0xFF00) | bs.praw), ppu->mirroring)];
                }
            } else if (ph == 7) {
                bs.phi = bgx_pat(ppu, base, bs.nt, bs.v, 1);
                v_inc_coarse_x(&bs.v);
            }
        }

        if (d > 256) continue;

        int bit = 15 - fx;
        uint8_t bgc = (uint8_t)(((bs.lo >> bit) & 1) | (((bs.hi >> bit) & 1) << 1));
        uint8_t bgp = (uint8_t)((((bs.ahi >> bit) & 1) << 1) | ((bs.alo >> bit) & 1));
        int bg_en = (m & 0x08) != 0;
        int bg_show = bg_en && !(x < 8 && !(m & 0x02));

        int sp_slot = -1;
        uint8_t sp_c = 0;
        int s0_opaque = 0;
        for (int k = 0; k < 8; k++) {
            if (!ss.halted[k]) {
                if (ss.cnt[k] == 0) ss.halted[k] = 1;
                else { ss.cnt[k]--; continue; }
            }
            uint8_t c = (uint8_t)(((ss.lo[k] >> 7) & 1) | (((ss.hi[k] >> 7) & 1) << 1));
            ss.lo[k] = (uint8_t)(ss.lo[k] << 1);
            ss.hi[k] = (uint8_t)(ss.hi[k] << 1);
            if (c) {
                if (k == 0 && ppu->spr0_line) s0_opaque = 1;
                if (sp_slot < 0) { sp_slot = k; sp_c = c; }
            }
        }

        int sp_en = (m & 0x10) != 0;
        int sp_show = sp_en && sp_slot >= 0 && !(x < 8 && !(m & 0x04));

        if (hit < 0 && s0_opaque && bgc && (m & 0x18) == 0x18 && x < 255 &&
            !(x < 8 && (!(m & 0x02) || !(m & 0x04)))) {
            hit = x;
        }

        if (draw) {
            uint8_t idx;
            uint8_t bgi = (bg_show && bgc) ? (uint8_t)((bgp << 2) | bgc) : 0;
            if (sp_show && (!bgi || !(ss.attr[sp_slot] & 0x20)))
                idx = (uint8_t)(0x10 + ((ss.attr[sp_slot] & 3) << 2) + sp_c);
            else
                idx = bgi;
            screen_buffer[ppu->scanline * 256 + x] = nes_color_to_1555(ppu->palette_ram[idx]);
        }
    }

    if (!draw) {
        if (hit_x) *hit_x = hit;
        return;
    }

    if (hit >= 0) ppu->reg_status |= 0x40;

    ppu->bgs = bs;
    ppu->sps = ss;
    ppu->bgs_run_valid = 1;
    ppu->bgs_run_dot = 257;
    ppu->bgs_on = (m & 0x18) != 0;
    ppu->bgs_pf_ok = 0;
}

static void bg_run_begin(ppu_t *ppu, int on)
{
    if (!(ppu->line_rendered == 0 && ppu->bgs_start_valid)) {
        ppu_bgstate_t *bs = &ppu->bgs;
        uint16_t base = (ppu->line_latched ? ppu->line_bg_hi : ((ppu->reg_ctrl & 0x10) != 0)) ? 0x1000 : 0x0000;
        uint16_t v = bg_v_at_tile(ppu, 32);
        uint8_t lo[2], hi[2], pal[2], nt = 0, raw = 0;
        for (int i = 0; i < 2; i++) {
            nt = bgx_nt(ppu, v);
            bgx_at(ppu, v, &pal[i], &raw);
            lo[i] = bgx_pat(ppu, base, nt, v, 0);
            hi[i] = bgx_pat(ppu, base, nt, v, 1);
            v_inc_coarse_x(&v);
        }
        bs->lo  = (uint16_t)((lo[0] << 8) | lo[1]);
        bs->hi  = (uint16_t)((hi[0] << 8) | hi[1]);
        bs->alo = (uint16_t)(((pal[0] & 1) ? 0xFF00 : 0) | ((pal[1] & 1) ? 0x00FF : 0));
        bs->ahi = (uint16_t)(((pal[0] & 2) ? 0xFF00 : 0) | ((pal[1] & 2) ? 0x00FF : 0));
        bs->nt = nt; bs->pal = pal[1]; bs->praw = raw; bs->plo = lo[1]; bs->phi = hi[1];
        bs->v = v;
    }
    ppu->bgs_run_valid = 1;
    ppu->bgs_run_dot = 257;
    ppu->bgs_on = on ? 1 : 0;
    ppu->bgs_pf_ok = 0;
}

static void bg_run_advance(ppu_t *ppu, int to)
{
    ppu_bgstate_t *bs = &ppu->bgs;
    uint16_t base = (ppu->reg_ctrl & 0x10) ? 0x1000 : 0x0000;
    for (int d = ppu->bgs_run_dot + 1; d <= to; d++) {
        if (!ppu->bgs_on) continue;
        if (d < 322 || d > 337) continue;

        bg_shift(bs);
        if (d == 329 || d == 337) {
            bg_reload(bs);
            ppu->bgs_pf_ok |= (d == 329) ? 1 : 2;
        }
        int ph = (d - 1) & 7;
        if (d == 322) bs->v = ppu->v;
        if (ph == 1)      { bs->nt = bgx_nt(ppu, bs->v); }
        else if (ph == 3) { bgx_at(ppu, bs->v, &bs->pal, &bs->praw); }
        else if (ph == 5) { bs->plo = bgx_pat(ppu, base, bs->nt, bs->v, 0); }
        else if (ph == 7) { bs->phi = bgx_pat(ppu, base, bs->nt, bs->v, 1); v_inc_coarse_x(&bs->v); }
    }
    if (to > ppu->bgs_run_dot) ppu->bgs_run_dot = to;
}

static void bg_hblank_event(ppu_t *ppu, int d, uint8_t old_mask, uint8_t new_mask)
{
    if (ppu->scanline >= 240) return;
    if (!ppu->bgs_run_valid) bg_run_begin(ppu, (old_mask & 0x18) != 0);
    if (d - 1 > ppu->bgs_run_dot) bg_run_advance(ppu, d - 1);
    ppu->bgs_on = (new_mask & 0x18) != 0;
}

static void sprite0_predict_dotwise(ppu_t *ppu)
{
    ppu->spr0_hit_cycle = 0;
    if (ppu->scanline >= 240 || ppu->line_rendered) return;
    if (!ppu->spr0_line || (ppu->reg_status & 0x40)) return;

    int hx;
    render_line_dotwise(ppu, 0, &hx);
    if (hx < 0) return;

    int hc = hx + PPU_SPR0_HIT_DELAY;
    if (hc < 2) hc = 2;
    if (hc <= ppu->cycle) hc = ppu->cycle + 1;
    ppu->spr0_hit_cycle = hc;
}

static int bg_record_mask_event(ppu_t *ppu, uint8_t old_mask, uint8_t new_mask)
{
    if (ppu->scanline >= 240 || ppu->cycle < 1) return 0;

    int vsl, vd;
    spr_virtual_dot(ppu->scanline, ppu->cycle, &vsl, &vd);
    int d = vd + PPU_MASK_WRITE_DELAY;
    int line = vsl;
    if (d > 340) { d -= 341; line++; }

    if (line == ppu->scanline) {
        if (d < 1) d = 1;
        if (!ppu->line_rendered) {
            if (ppu->rt_count < 6) {
                ppu->rt_dot[ppu->rt_count] = (int16_t)d;
                ppu->rt_mask[ppu->rt_count] = new_mask;
                ppu->rt_count++;
                return 1;
            }
            return 0;
        }
        bg_hblank_event(ppu, d, old_mask, new_mask);
        return 0;
    }
    if (line == ppu->scanline + 1 && line < 240) {
        ppu->rt_pend = 1;
        ppu->rt_pend_dot = (int16_t)(d < 1 ? 1 : d);
        ppu->rt_pend_new = new_mask;
        ppu->rt_pend_old = old_mask;
    }
    return 0;
}

static uint16_t bus_v_at(uint16_t v0, int k)
{
    uint16_t v = v0;
    for (int i = 0; i < k; i++) v_inc_coarse_x(&v);
    return v;
}

static int ppu_bus_value_at(const ppu_t *ppu, int sl, int vd, uint16_t v0, int v0_is_hblank,
                            uint16_t base, uint8_t *out)
{
    if (vd < 1 || vd > 340) return 0;
    if (v0_is_hblank && vd <= 256) return 0;

    if (vd <= 256) {
        int ph = (vd - 1) & 7;
        if (!(ph & 1)) return 0;
        uint16_t v = bus_v_at(v0, ((vd - 1) >> 3) + 2);
        uint8_t nt = bgx_nt(ppu, v);
        if (ph == 1) { *out = nt; return 1; }
        if (ph == 3) { uint8_t pal, raw; bgx_at(ppu, v, &pal, &raw); *out = raw; return 1; }
        *out = bgx_pat(ppu, base, nt, v, ph == 7);
        return 1;
    }

    uint16_t v_old, v_new;
    if (v0_is_hblank) {
        v_old = v_new = v0;
    } else {
        v_old = bus_v_at(v0, 34);
        v_new = v_old;
        v_inc_y(&v_new);
        v_new = (uint16_t)((v_new & ~0x041F) | (ppu->t & 0x041F));
    }

    if (vd <= 320) {
        int off = (vd - 257) & 7, slot = (vd - 257) >> 3;
        if (!(off & 1)) return 0;
        if (off == 1) { *out = bgx_nt(ppu, slot == 0 ? v_old : v_new); return 1; }
        if (off == 3) { *out = bgx_nt(ppu, v_new); return 1; }
        const uint8_t *e = &ppu->sec_oam[slot * 4];
        int height = (ppu->reg_ctrl & 0x20) ? 16 : 8;
        int row = (sl - (int)e[0]) & (height - 1);
        if (e[2] & 0x80) row = (height - 1) - row;
        uint16_t a;
        if (height == 16) {
            uint8_t t = e[1] & 0xFE;
            if (row >= 8) { t++; row &= 7; }
            a = (uint16_t)(((e[1] & 1) ? 0x1000 : 0) + t * 16 + row);
        } else {
            a = (uint16_t)(((ppu->reg_ctrl & 0x08) ? 0x1000 : 0) + e[1] * 16 + row);
        }
        *out = ppu->vram[(a + (off == 7 ? 8 : 0)) & 0x1FFF];
        return 1;
    }

    if (vd <= 336) {
        int ph = (vd - 321) & 7, g = (vd - 321) >> 3;
        if (!(ph & 1)) return 0;
        uint16_t v = v_new;
        for (int i = 0; i < g; i++) v_inc_coarse_x(&v);
        uint8_t nt = bgx_nt(ppu, v);
        if (ph == 1) { *out = nt; return 1; }
        if (ph == 3) { uint8_t pal, raw; bgx_at(ppu, v, &pal, &raw); *out = raw; return 1; }
        *out = bgx_pat(ppu, base, nt, v, ph == 7);
        return 1;
    }
    if (vd == 338 || vd == 340) {
        uint16_t v = v_new; v_inc_coarse_x(&v); v_inc_coarse_x(&v);
        *out = bgx_nt(ppu, v);
        return 1;
    }
    return 0;
}

static void ppu_2007_buffer_from_bus(ppu_t *ppu)
{
    if (!(ppu->reg_mask & 0x18) || ppu->scanline >= 240) return;

    int vsl, vd;
    spr_virtual_dot(ppu->scanline, ppu->cycle, &vsl, &vd);
    vd += PPU_2007_BUFFER_DELAY;
    if (vd > 340) { vd -= 341; vsl++; }

    int snapshot_ready = (ppu->cycle >= 2) && ppu->line_latched;

    uint16_t v0, base;
    int v0_is_hblank = 0;
    if (vsl == ppu->scanline && snapshot_ready) {
        v0 = ppu->line_v;
        base = ppu->line_bg_hi ? 0x1000 : 0x0000;
    } else if ((vsl == ppu->scanline + 1 && vsl < 240) ||
               (vsl == ppu->scanline && ppu->cycle < 2)) {
        v0 = ppu->v;
        base = (ppu->reg_ctrl & 0x10) ? 0x1000 : 0x0000;
    } else if (vsl == ppu->scanline && ppu->cycle >= 258 && vd > 256) {
        v0 = ppu->v;
        v0_is_hblank = 1;
        base = (ppu->reg_ctrl & 0x10) ? 0x1000 : 0x0000;
    } else {
        return;
    }
    uint8_t bv;
    if (ppu_bus_value_at(ppu, vsl, vd, v0, v0_is_hblank, base, &bv))
        ppu->vram_read_buffer = bv;
}

static void sps_end_of_fast_line(ppu_t *ppu)
{
    for (int k = 0; k < 8; k++) {
        int s = 256 - (int)ppu->sps.cnt[k];
        if (s >= 8) { ppu->sps.lo[k] = 0; ppu->sps.hi[k] = 0; }
        else if (s > 0) {
            ppu->sps.lo[k] = (uint8_t)(ppu->sps.lo[k] << s);
            ppu->sps.hi[k] = (uint8_t)(ppu->sps.hi[k] << s);
        }
        ppu->sps.cnt[k] = 0;
        ppu->sps.halted[k] = 1;
    }
}

/* ============================================================
 *  Satır çizimi (dot 256'da)
 * ============================================================ */
static void render_scanline(ppu_t *ppu) {
    if (ppu->scanline >= 240) return;

    ppu->line_rendered = 1;
    if (PPU_DOTWISE_ALWAYS || ppu->rt_count || ppu->bgs_start_valid || ppu->sps_irregular) {
        render_line_dotwise(ppu, 1, NULL);
        {
            uint8_t prev = ppu->rt_mask0;
            for (int i = 0; i < ppu->rt_count; i++) {
                if (ppu->rt_dot[i] > 257)
                    bg_hblank_event(ppu, ppu->rt_dot[i], prev, ppu->rt_mask[i]);
                prev = ppu->rt_mask[i];
            }
        }
        return;
    }

    uint8_t bg_pixels[256];
    render_bg_row(ppu, bg_pixels, 1, 0);

    if (!(ppu->reg_mask & 0x10)) { sps_end_of_fast_line(ppu); return; }

    uint8_t covered[256];
    memset(covered, 0, sizeof(covered));

    bool both_on = (ppu->reg_mask & 0x18) == 0x18;
    bool left_clip = !(ppu->reg_mask & 0x02) || !(ppu->reg_mask & 0x04);
    bool hit = false;

    for (int k = 0; k < 8; k++) {
        if (!ppu->spr_valid[k]) continue;

        uint8_t attr = ppu->spr_attr[k];
        uint8_t p0 = ppu->spr_lo[k];
        uint8_t p1 = ppu->spr_hi[k];
        int sx = ppu->spr_x[k];
        bool flip_x = (attr & 0x40) != 0;
        bool bg_priority = (attr & 0x20) != 0;

        for (int px = 0; px < 8; px++) {
            int draw_x = sx + px;
            if (draw_x >= 256) break;

            uint8_t bit = flip_x ? px : (7 - px);
            uint8_t color = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
            if (color == 0) continue;

            if (k == 0 && ppu->spr0_line && both_on &&
                bg_pixels[draw_x] != 0 && draw_x < 255) {
                int age_at_pixel = ppu->render_age - 1 - (256 - (draw_x + 1));
                if (!(draw_x < 8 && left_clip) && age_at_pixel >= PPU_BG_FILL_DOTS) hit = true;
            }

            if (covered[draw_x]) continue;
            covered[draw_x] = 1;

            if (bg_priority && bg_pixels[draw_x] != 0) continue;

            screen_buffer[ppu->scanline * 256 + draw_x] =
                nes_color_to_1555(ppu->palette_ram[0x10 + (attr & 0x03) * 4 + color]);
        }
    }

    if (hit) ppu->reg_status |= 0x40;
    sps_end_of_fast_line(ppu);
}

void ppu_init(ppu_t *ppu) {
    memset(ppu, 0, sizeof(*ppu));
    for (int i = 0; i < 32; i++) {
        ppu->palette_ram[i] = nintendo_power_up_palette[i];
    }
#if EXP_FAST_BLIT
    ppu_blit_init();
#endif
}

void ppu_reset(ppu_t *ppu) {
    ppu->scanline = 0; ppu->cycle = 0; ppu->v = 0; ppu->t = 0; ppu->w = 0;
    ppu->reg_status = 0;
    /* DÜZELTME: ppu->nmi_requested = 0; satırı kaldırıldı */
    ppu->open_bus = 0; ppu->open_bus_decay_timer = 0;
    ppu->vbl_suppress = false;
    ppu->odd_skip_rendering = 0;
    ppu->odd_skip_latched = 0;
    ppu->frame = 0;

    memset(ppu->sec_oam, 0xFF, sizeof(ppu->sec_oam));
    ppu->oam_buffer = 0xFF;
    ppu->eval_phase = EVAL_RANGE;
    ppu->eval_ptr = 0;
    ppu->eval_found = 0;
    ppu->eval_copy_left = 0;
    ppu->eval_ovf_left = 0;
    ppu->spr0_sec = 0;
    ppu->spr0_line = 0;
    memset(ppu->spr_valid, 0, sizeof(ppu->spr_valid));
    ppu->spr0_hit_cycle = 0;
    ppu->ovf_set_cycle = 0;
    ppu->oam2_addr = 0;
    ppu->oam2_frozen = 0;
    memset(ppu->fetch_byte, 0, sizeof(ppu->fetch_byte));
    ppu->bg_ev_count = 0;
    memset(&ppu->bgs, 0, sizeof(ppu->bgs));
    memset(&ppu->sps, 0, sizeof(ppu->sps));
    ppu->bgs_start_valid = 0; ppu->bgs_run_valid = 0; ppu->bgs_pf_ok = 0; ppu->bgs_on = 0;
    ppu->bgs_run_dot = 0; ppu->sps_irregular = 0; ppu->line_rendered = 0;
    ppu->pre_pf = 3;
    ppu->rt_count = 0; ppu->rt_mask0 = 0; ppu->rt_pend = 0;
    ppu->render_prev = 0;
    ppu->render_age = 0;
    ppu->oam_corrupt_pending = 0;
    ppu->oam_corrupt_row = 0;
    ppu->line_latched = 0;
}

int ppu_nmi_line(const ppu_t *ppu)
{
    return ppu &&
           (ppu->reg_ctrl & 0x80u) &&
           (ppu->reg_status & 0x80u);
}

uint8_t ppu_cpu_read(ppu_t *ppu, uint16_t addr) {
    uint8_t res = ppu->open_bus;

    switch (addr & 7) {
        case 0:
        case 1:
        case 3:
        case 5:
        case 6:
            return ppu->open_bus;

        case 2:
            res = (ppu->reg_status & 0xE0) | (ppu->open_bus & 0x1F);

            if (ppu->scanline == 241 && ppu->cycle <= 3) {
                res |= 0x80;
                ppu->vbl_suppress = true;
                /* DÜZELTME: ppu->nmi_requested = 0; satırı kaldırıldı */
            }
            if (ppu->scanline == 240 && ppu->cycle >= 340) {
                ppu->vbl_suppress = true;
            }

            ppu->reg_status &= ~0x80;
            ppu->w = 0;

            ppu->open_bus = res;
            return res;

        case 4:
            {
                int rc = ppu->cycle - 1, rsl = ppu->scanline;
                if (rc < 0) { rc = 340; rsl = (rsl == 0) ? 261 : rsl - 1; }
                int vsl, vd;
                spr_virtual_dot(rsl, rc, &vsl, &vd);

                if ((ppu->reg_mask & 0x18) && (vsl < 240 || vsl == 261)) {
                    if (vd >= 1 && vd <= 64)        res = 0xFF;
                    else if (vd >= 65 && vd <= 320) res = ppu->oam_buffer;
                    else                            res = ppu->sec_oam[ppu->oam2_frozen ? 0 : (ppu->oam2_addr & 31)];
                } else {
                    res = oam_read_masked(ppu, ppu->oam_addr);
                }
            }
            break;

        case 7: {
            uint16_t old_v = ppu->v;
            uint16_t v_addr = ppu->v & 0x3FFF;
            uint8_t current_buffer = ppu->vram_read_buffer;

            if (v_addr < 0x3F00) {
                ppu->vram_read_buffer = ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)];
                res = current_buffer;
            } else {
                ppu->vram_read_buffer = ppu->vram[ppu_core_vram_mask(v_addr - 0x1000, ppu->mirroring)];
                res = ppu->palette_ram[ppu_core_palette_mirror_index(v_addr & 0x1F)];
                if (ppu->reg_mask & 0x01) {
                    res &= 0x30;
                }
                res = (res & 0x3F) | (ppu->open_bus & 0xC0);
            }

            bg_note_2007_read(ppu);
            ppu_2007_buffer_from_bus(ppu);
            ppu_advance_v_after_data_access(ppu);
            ppu_a12_check(old_v, ppu->v);
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
            /* DÜZELTME: nmi_requested set bloğu tamamen kaldırıldı */
            ppu->reg_ctrl = val;
            ppu->t = (ppu->t & ~0x0C00) | ((val & 0x03) << 10);
        } break;
        case 1: {
            uint8_t old_mask = ppu->reg_mask;
            ppu->reg_mask = val;
            if ((val ^ old_mask) & 0x18) {
                int rec = bg_record_mask_event(ppu, old_mask, val);
                if (rec && ppu->cycle <= 253) sprite0_predict_dotwise(ppu);
            }
        } break;
        case 3: ppu->oam_addr = val; break;
        case 4:
#if PPU_BLOCK_OAM_WRITE_WHILE_RENDERING
            if (ppu_rendering_line(ppu)) {
                ppu->oam_addr = (uint8_t)((ppu->oam_addr + 4) & 0xFC);
                break;
            }
#endif
            ppu->oam[ppu->oam_addr++] = val;
            break;
        case 5:
            if (ppu->w == 0) { ppu->t = (ppu->t & ~0x001F) | (val >> 3); ppu->x = val & 0x07; ppu->w = 1; }
            else { ppu->t = (ppu->t & ~0x73E0) | ((val & 0x07) << 12) | ((val & 0xF8) << 2); ppu->w = 0; }
            break;
        case 6:
            if (ppu->w == 0) {
                ppu->t = (ppu->t & 0x00FF) | ((val & 0x3F) << 8);
                ppu->w = 1;
            } else {
                uint16_t old_v = ppu->v;
                ppu->t = (ppu->t & 0xFF00) | val;
                ppu->v = ppu->t;
                ppu->w = 0;
                ppu_a12_check(old_v, ppu->v);
                bg_note_v_write(ppu, ppu->v);
            }
            break;
        case 7: {
            uint16_t old_v = ppu->v;
            uint16_t v_addr = ppu->v & 0x3FFF;
            if (v_addr >= 0x3F00) {
                ppu->palette_ram[ppu_core_palette_mirror_index(v_addr & 0x1F)] = val;
            } else if (v_addr < 0x2000) {
                if (!nes_chr_is_rom) {
                    ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)] = val;
                }
            } else {
                ppu->vram[ppu_core_vram_mask(v_addr, ppu->mirroring)] = val;
            }
            ppu_advance_v_after_data_access(ppu);
            ppu_a12_check(old_v, ppu->v);
        } break;
    }
}

/* Tek PPU dot'u. Eskiden ppu_step_cycles() dongusunun govdesiydi; davranisi AYNIDIR. */
static inline __attribute__((always_inline)) void ppu_dot(ppu_t *ppu) {
    {
        ppu->open_bus_decay_timer++;
        if (ppu->open_bus_decay_timer > 5369319) {
            ppu->open_bus = 0x00;
            ppu->open_bus_decay_timer = 0;
        }


        /* ---- HIZLI YOLLAR (davranis ORIJINAL govdeyle BIREBIR aynidir) ----
         * Asagidaki iki blok, orijinal govdenin belirli, cok yaygin dot'larda yaptigi isin
         * kisa devre halidir. Kosullar saglanmazsa hicbir sey degismez ve orijinal govde
         * calisir. Dogrulama: ozgun ppu.c ile diferansiyel test. */

        /* A) Gorunur scanline (0..239), duragan render, "siradan" dot.
         *   - render_age doymus (255): artis yok, rendering_v dogru
         *   - render_prev==1 && !oam_corrupt_pending: OAM bozulma blogu etkisiz
         *   - cycle 1/3/256/257/258 ozel dot'lar (satir basi, A12 saati, render_scanline,
         *     v kopyalama) yavas yola birakilir
         *   - bekleyen ovf/spr0 bayrak olayi varsa yavas yol */
        if (ppu->scanline < 240 &&
            (ppu->reg_mask & 0x18) &&
            ppu->render_age == 255 &&
            ppu->render_prev &&
            !ppu->oam_corrupt_pending &&
            !ppu->ovf_set_cycle && !ppu->spr0_hit_cycle) {
            const int cyc = ppu->cycle;
            if (cyc != 1 && cyc != 3 && cyc != 256 && cyc != 257 && cyc != 258) {
                int vsl, vd;
                spr_virtual_dot(ppu->scanline, cyc, &vsl, &vd);

                if (vd == 339 && (vsl < 240 || vsl == 261)) {
                    for (int k = 0; k < 8; k++) ppu->sps.halted[k] = 0;
                    ppu->sps_irregular = 0;
                }

                if (EXP_SPR_PIPE_ON && (vsl < 240 || vsl == 261)) {
                    if (vd == 63 || vd == 255 || vd == 339) ppu->oam2_frozen = 0;

                    if (vd >= 1 && vd <= 64) {
                        sprite_clear_dot(ppu, vd);
                    } else if (vd >= 65 && vd <= 256) {
                        if (vsl < 240) sprite_eval_dot(ppu, vsl, vd);
                    } else if (vd >= 257 && vd <= 320) {
                        sprite_fetch_dot(ppu, vsl, vd);
                    }

                    if (vd >= 257 && vd <= 320) ppu->oam_addr = 0;
                }

                if (++ppu->cycle >= 341) {
                    ppu->cycle = 0;
                    ppu->scanline++;
                }

                /* sprite_eval_dot ovf_set_cycle kurduysa orijinal govde bunu ayni dot'ta,
                 * cycle++ sonrasi isler. */
                if (ppu->ovf_set_cycle && ppu->cycle == ppu->ovf_set_cycle &&
                    ppu->scanline < 240) {
                    ppu->reg_status |= 0x20;
                    ppu->ovf_set_cycle = 0;
                }
                return;
            }
        }

        /* C) Gorunur scanline, render KAPALI ve duragan kapali (render_age==0, render_prev==0):
         *    orijinal govde yalniz sayaci ilerletir. Ozel dot'lar yavas yolda kalir:
         *    cycle 1 (satir basi blogu), 256 (render_scanline/rt_count), 336 (sps_irregular). */
        if (ppu->scanline < 240 && !(ppu->reg_mask & 0x18) &&
            ppu->render_age == 0 && !ppu->render_prev &&
            !ppu->ovf_set_cycle && !ppu->spr0_hit_cycle) {
            const int cyc = ppu->cycle;
            if (cyc != 1 && cyc != 256 && cyc != 336) {
                if (++ppu->cycle >= 341) {
                    ppu->cycle = 0;
                    ppu->scanline++;
                }
                return;
            }
        }

        /* B) Scanline 240..259 (VBL dahil), (241, cycle 1) VBL-set dot'u haric:
         *    tek etki render_age/render_prev guncellemesi ve sayac ilerlemesidir. */
        if (ppu->scanline >= 240 && ppu->scanline <= 259 &&
            !(ppu->scanline == 241 && ppu->cycle == 1)) {
            if (ppu->reg_mask & 0x18) {
                if (ppu->render_age < 255) ppu->render_age++;
                ppu->render_prev = 1;
            } else {
                ppu->render_age = 0;
                ppu->render_prev = 0;
            }
            if (++ppu->cycle >= 341) {
                ppu->cycle = 0;
                ppu->scanline++;
            }
            return;
        }

        bool rendering = (ppu->reg_mask & 0x18) != 0;
        int render_age_before = ppu->render_age;
        if (rendering) { if (ppu->render_age < 255) ppu->render_age++; }
        else           { ppu->render_age = 0; }
        bool rendering_v = rendering && render_age_before >= PPU_MASK_ENABLE_DELAY;

#if PPU_EMULATE_OAM_CORRUPTION
        {
            bool line_ok = (ppu->scanline < 240 || ppu->scanline == 261);
            if (ppu->render_prev && !rendering && line_ok) {
                {
                    int cvsl, cvd;
                    spr_virtual_dot(ppu->scanline, ppu->cycle, &cvsl, &cvd);
                    ppu->oam_corrupt_row = (uint8_t)oam_corrupt_seed(ppu, cvd);
                }
                ppu->oam_corrupt_pending = 1;
            }
            if (rendering && line_ok && ppu->oam_corrupt_pending) {
                oam_corrupt_apply(ppu);
                ppu->oam_corrupt_pending = 0;
            }
            ppu->render_prev = rendering ? 1 : 0;
        }
#endif

        if (ppu->scanline == 261 && ppu->cycle == 336) {
            ppu->odd_skip_rendering = rendering ? 1u : 0u;
            ppu->odd_skip_latched = 1u;
        }

        if (ppu->scanline == 261 &&
            ppu->cycle == 339 &&
            (ppu->frame % 2 != 0) &&
            ppu->odd_skip_latched &&
            ppu->odd_skip_rendering) {
            ppu->cycle = 0;
            ppu->scanline = 0;
            ppu->frame++;
            ppu->vbl_suppress = false;
            ppu->reg_status &= ~0xE0;
            ppu->odd_skip_latched = 0;
            return;
        }

        if (ppu->cycle == 1 && ppu->scanline < 240) {
            ppu->spr0_hit_cycle = 0;
            ppu->ovf_set_cycle = 0;
            ppu->bg_ev_count = 0;

            ppu->line_rendered = 0;
            ppu->rt_count = 0;
            ppu->rt_mask0 = ppu->reg_mask;
            if (ppu->rt_pend) {
                ppu->rt_mask0 = ppu->rt_pend_old;
                ppu->rt_dot[0] = ppu->rt_pend_dot;
                ppu->rt_mask[0] = ppu->rt_pend_new;
                ppu->rt_count = 1;
                ppu->rt_pend = 0;
            }
            if (ppu->scanline == 0) {
                ppu->bgs_run_valid = 0;
                ppu->bgs_start_valid = (ppu->pre_pf != 3) ? 1 : 0;
                if (ppu->bgs_start_valid) ppu->bgs.v = ppu->v;
            } else if (ppu->bgs_run_valid) {
                int reached = ppu->bgs_run_dot;
                bg_run_advance(ppu, 340);
                ppu->bgs_start_valid = (ppu->bgs_pf_ok != 3) ? 1 : 0;
                ppu->bgs_run_valid = 0;
                if (ppu->bgs_start_valid && reached < 322) ppu->bgs.v = ppu->v;
            }

            if (rendering) {
                ppu->line_latched = 1;
                ppu->line_v = ppu->v;
                ppu->line_x = ppu->x;
                ppu->line_bg_hi = (ppu->reg_ctrl & 0x10) ? 1 : 0;
                if (PPU_DOTWISE_ALWAYS || ppu->rt_count || ppu->bgs_start_valid || ppu->sps_irregular)
                    sprite0_predict_dotwise(ppu);
                else
                    sprite0_schedule_hit(ppu, 1);
            } else {
                ppu->line_latched = 0;
            }
        }

        if (EXP_LINE_RENDER_ON && ppu->cycle == 256 && (rendering || ppu->rt_count) && ppu->scanline < 240) {
            render_scanline(ppu);
        }

        {
            int vsl, vd;
            spr_virtual_dot(ppu->scanline, ppu->cycle, &vsl, &vd);

            if (vsl == 261) {
                if (vd == 322) ppu->pre_pf = 0;
                if (rendering) {
                    if (vd == 329) ppu->pre_pf |= 1;
                    if (vd == 337) ppu->pre_pf |= 2;
                }
            }

            if (vd == 339 && (vsl < 240 || vsl == 261)) {
                if (rendering) {
                    for (int k = 0; k < 8; k++) ppu->sps.halted[k] = 0;
                    ppu->sps_irregular = 0;
                } else {
                    ppu->sps_irregular = 1;
                }
            }

            if (rendering && EXP_SPR_PIPE_ON && (vsl < 240 || vsl == 261)) {
                if (vd == 63 || vd == 255 || vd == 339) ppu->oam2_frozen = 0;

                if (vd >= 1 && vd <= 64) {
                    sprite_clear_dot(ppu, vd);
                } else if (vd >= 65 && vd <= 256) {
                    if (vsl < 240) sprite_eval_dot(ppu, vsl, vd);
                } else if (vd >= 257 && vd <= 320) {
                    sprite_fetch_dot(ppu, vsl, vd);
                }

                if (vd >= 257 && vd <= 320) ppu->oam_addr = 0;
            }
        }

        if (rendering && (ppu->scanline < 240 || ppu->scanline == 261)) {
            int bg_bit  = (ppu->reg_ctrl & 0x10) ? 1 : 0;
            int spr_bit = (ppu->reg_ctrl & 0x08) ? 1 : 0;

            if ((bg_bit == 0 && spr_bit == 1 && ppu->cycle == 258) ||
                (bg_bit == 1 && spr_bit == 0 && ppu->cycle == 3))
                mapper_scanline_clock();
        }

        if (rendering_v && (ppu->scanline < 240 || ppu->scanline == 261)) {
            if (ppu->cycle == 256) v_inc_y(&ppu->v);
            if (ppu->cycle == 257) ppu->v = (ppu->v & ~0x041F) | (ppu->t & 0x041F);
            if (ppu->scanline == 261 && ppu->cycle >= 280 && ppu->cycle <= 304) ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
        }

        ppu->cycle++;
        if (ppu->cycle >= 341) {
            ppu->cycle = 0;
            ppu->scanline++;

            if (ppu->scanline == 261) {
                ppu->reg_status &= ~0xE0;
                /* DÜZELTME: ppu->nmi_requested = 0; satırı kaldırıldı */
                ppu->vbl_suppress = false;
            } else if (ppu->scanline >= 262) {
                ppu->scanline = 0; ppu->frame++; ppu->vbl_suppress = false;
                ppu->odd_skip_latched = 0;
            }
        }

        if (ppu->scanline == 260 && ppu->cycle == 341 - PPU_SPRITE_FLAG_CLEAR_LEAD) {
            ppu->reg_status &= ~0x60;
        }

        if (ppu->ovf_set_cycle && ppu->cycle == ppu->ovf_set_cycle &&
            ppu->scanline < 240) {
            ppu->reg_status |= 0x20;
            ppu->ovf_set_cycle = 0;
        }

        if (ppu->spr0_hit_cycle && ppu->cycle == ppu->spr0_hit_cycle &&
            ppu->scanline < 240) {
            ppu->reg_status |= 0x40;
            ppu->spr0_hit_cycle = 0;
        }

        if (ppu->scanline == 241 && ppu->cycle == 2) {
            if (!ppu->vbl_suppress) {
                ppu->reg_status |= 0x80;
                /* DÜZELTME: if (ppu->reg_ctrl & 0x80) ppu->nmi_requested = 1;
                 * bloğu tamamen kaldırıldı. NMI seviyesi ppu_nmi_line() ile
                 * örnekleniyor (reg_ctrl bit 7 & reg_status bit 7). */
            }
        }
    }
}


/* ---- SPRITE PIPELINE TOPLU YOLLARI (davranis ppu_dot ile BIREBIR aynidir) ----
 * Hizli yol A kosullari saglaniyorsa ve dot'lar "siradan" ise, bir dizi dot'u tek
 * seferde calistirir. Koşullar ppu_dot'taki hizli yol A ile aynidir; ek olarak sprite
 * pipeline'in belirli, yan etkisi basit fazlari kapali formda islenir. */

/* open_bus bozunma sayacini d dot ilerlet (ppu_dot basindaki per-dot mantigin kapali formu). */
static inline void ppu_decay_advance(ppu_t *ppu, uint32_t d)
{
    const uint32_t T = 5369319u;
    uint32_t t = ppu->open_bus_decay_timer;
    if (t + d <= T) {
        ppu->open_bus_decay_timer = t + d;
    } else {
        const uint32_t j = T + 1u - t;     /* esik asimi: bu dot'ta sayac 0'lanir, open_bus = 0 */
        ppu->open_bus = 0x00;
        ppu->open_bus_decay_timer = d - j;
    }
}

static inline __attribute__((always_inline)) uint32_t ppu_sprite_bulk(ppu_t *ppu, uint32_t maxd)
{
    if (ppu->scanline >= 240 || !(ppu->reg_mask & 0x18) ||
        ppu->render_age != 255 || !ppu->render_prev ||
        ppu->oam_corrupt_pending || ppu->ovf_set_cycle || ppu->spr0_hit_cycle) {
        return 0;
    }

    const int cyc = ppu->cycle;

#if EXP_ABLATE
    if (!EXP_SPR_PIPE_ON) {
        /* M2/M4 (SADECE OLCUM): sprite pipeline kapali; hizli yol A'da siradan dot'lar
         * yalniz sayac + bozunma ilerletir. */
        uint32_t d;
        if (cyc >= 4 && cyc <= 255)        d = (uint32_t)(256 - cyc);
        else if (cyc >= 259 && cyc <= 335) d = (uint32_t)(336 - cyc);
        else if (cyc >= 337)               d = (uint32_t)(341 - cyc);
        else return 0;
        if (d > maxd) d = maxd;
        int c2 = cyc + (int)d;
        if (c2 >= 341) { c2 = 0; ppu->scanline++; }
        ppu->cycle = c2;
        ppu_decay_advance(ppu, d);
        return d;
    }
#endif

    /* (1) Ikincil OAM temizleme: cycle 4..61 (vd 7..64). cycle 1/3 ozel oldugundan 4'ten baslar. */
    if (cyc >= 4 && cyc <= 61) {
        uint32_t d = (uint32_t)(61 - cyc + 1);
        if (d > maxd) d = maxd;
        if (d == 0) return 0;

        const int v0 = cyc + PPU_SPRITE_PIPELINE_LEAD;
        const int v1 = v0 + (int)d - 1;
        for (int i = (v0 - 1) >> 1; i <= ((v1 - 1) >> 1); i++) ppu->sec_oam[i] = 0xFF;
        ppu->oam_buffer = 0xFF;
        if (v0 <= 63 && v1 >= 63) ppu->oam2_frozen = 0;

        ppu->cycle = cyc + (int)d;
        ppu_decay_advance(ppu, d);
        return d;
    }

    /* (2) Menzil disi sprite taramasi: tek dot'ta baslayan (vd tek, 67..253) tam cift'ler. */
    if (cyc >= 64 && cyc <= 250 && !(cyc & 1) && ppu->eval_phase == EVAL_RANGE) {
        const int vd0 = cyc + PPU_SPRITE_PIPELINE_LEAD;           /* tek */
        uint32_t pairs_max = (uint32_t)((253 - vd0) / 2 + 1);
        if (pairs_max > maxd / 2u) pairs_max = maxd / 2u;
        if (pairs_max == 0) return 0;

        const int sl = ppu->scanline;
        const int height = (ppu->reg_ctrl & 0x20) ? 16 : 8;
        int addr = ppu->oam_addr;
        uint8_t last = 0;
        uint32_t pairs = 0;
        int done = 0;

        while (pairs < pairs_max) {
            const uint8_t sy = oam_read_masked(ppu, (uint8_t)addr);
            if (spr_in_range(sl, sy, height)) break;             /* menzil icinde: normal yol */
            last = sy;
            pairs++;
            int next = addr + 4;
            if (next > 0xFF) { done = 1; addr = (next & 0xFF) & 0xFC; break; }
            addr = next & 0xFC;
        }
        if (pairs == 0) return 0;

        if (ppu->eval_ptr < 32) ppu->sec_oam[ppu->eval_ptr] = last;
        ppu->oam_buffer = last;
        ppu->oam_addr = (uint8_t)addr;
        if (done) ppu->eval_phase = EVAL_DONE;

        const uint32_t d = pairs * 2u;
        ppu->cycle = cyc + (int)d;
        ppu_decay_advance(ppu, d);
        return d;
    }

    return 0;
}

/* Toplu yol dagitici. Donus: calistirilan dot sayisi (0 = hicbiri; normal ppu_dot'a don).
 *  - gorunur scanline + render ACIK : sprite temizleme / menzil disi tarama (ppu_sprite_bulk)
 *  - gorunur scanline + render KAPALI (duragan): ozel dot'lar (1, 256, 336) haric sayac ilerletme
 *  - scanline 240..259 (duragan acik/kapali): (241, cycle<=1) haric satir sonuna kadar sayac ilerletme
 * Hepsi hizli yol A/B/C'nin ardisik dot'lara uygulanmasinin kapali formudur. */
static __attribute__((noinline)) uint32_t ppu_bulk(ppu_t *ppu, uint32_t maxd)
{
#if EXP_ABLATE
    if (g_exp_mode >= 5) return 0;
#endif
    const int sl = ppu->scanline;
    const int cyc = ppu->cycle;

    if (sl < 240) {
        if (ppu->ovf_set_cycle || ppu->spr0_hit_cycle) return 0;

        if (ppu->reg_mask & 0x18) {
            return ppu_sprite_bulk(ppu, maxd);
        }

        /* render kapali, duragan */
        if (ppu->render_age != 0 || ppu->render_prev) return 0;

        uint32_t d;
        if (cyc >= 2 && cyc <= 255)        d = (uint32_t)(256 - cyc);
        else if (cyc >= 257 && cyc <= 335) d = (uint32_t)(336 - cyc);
        else if (cyc >= 337)               d = (uint32_t)(341 - cyc);
        else return 0;                     /* 0, 1, 256, 336: normal yol */
        if (d > maxd) d = maxd;

        int c2 = cyc + (int)d;
        if (c2 >= 341) { c2 = 0; ppu->scanline = sl + 1; }
        ppu->cycle = c2;
        ppu_decay_advance(ppu, d);
        return d;
    }

    if (sl <= 259) {
        if (sl == 241 && cyc <= 1) return 0;                 /* VBL-set dot'u normal yolda */
        if (ppu->reg_mask & 0x18) {
            if (ppu->render_age != 255 || !ppu->render_prev) return 0;
        } else {
            if (ppu->render_age != 0 || ppu->render_prev) return 0;
        }

        uint32_t d = (uint32_t)(341 - cyc);
        if (d > maxd) d = maxd;

        int c2 = cyc + (int)d;
        if (c2 >= 341) { c2 = 0; ppu->scanline = sl + 1; }
        ppu->cycle = c2;
        ppu_decay_advance(ppu, d);
        return d;
    }

    return 0;
}

/* ppu_bulk'un sifirdan farkli donebilecegi durumlarin UST-KUMESI (ucuz, satir ici on filtre):
 * render kapali veya scanline >= 240 her zaman aday; render aciksa yalniz sprite toplu
 * yollarinin cycle araliklari. */
static inline int ppu_bulk_candidate(const ppu_t *ppu)
{
    if (!(ppu->reg_mask & 0x18) || ppu->scanline >= 240) return 1;
#if EXP_ABLATE
    if (!EXP_SPR_PIPE_ON) return 1;
#endif
    const int c = ppu->cycle;
    return (c >= 4 && c <= 61) ||
           (c >= 64 && c <= 250 && !(c & 1) && ppu->eval_phase == EVAL_RANGE);
}

static inline int ppu_nmi_level_inline(const ppu_t *ppu) {
    return ((ppu->reg_ctrl & 0x80u) && (ppu->reg_status & 0x80u)) ? 1 : 0;
}

/* cycles dot'u adimlar. nmi_last != NULL ise emu_driver'in eski ornekleme desenini
 * AYNEN uygular: her 3 dot'luk grupta 1. ve 3. dot'tan sonra NMI seviyesi orneklenir
 * (yukselen kenarda *nmi_pending = 1), artan dot'larda ornekleme yoktur. */
static __attribute__((noinline)) void ppu_run(
    ppu_t *ppu, uint32_t cycles, uint8_t *nmi_last, int *nmi_pending)
{
    const uint32_t grouped = nmi_last ? (cycles - (cycles % 3u)) : 0u;
    uint32_t k = 0;

    for (uint32_t i = 0; i < cycles; ) {
        /* Toplu yollar: NMI seviyesi orneklemeyi etkilemeyecekse (seviye == son ornek; bu
         * dot'lar boyunca seviye degisemez) bir dizi dot tek seferde calistirilir. */
        if (cycles - i >= 2u && ppu_bulk_candidate(ppu) &&
            (!nmi_last || ppu_nmi_level_inline(ppu) == (int)*nmi_last)) {
            const uint32_t d = ppu_bulk(ppu, cycles - i);
            if (d) {
                if (i < grouped) {
                    uint32_t in_g = grouped - i;
                    if (in_g > d) in_g = d;
                    k = (k + in_g) % 3u;
                }
                i += d;
                continue;
            }
        }

        ppu_dot(ppu);

        if (i < grouped) {
            if (k != 1u) {
                uint8_t level = (uint8_t)ppu_nmi_level_inline(ppu);
                if (level && !*nmi_last) {
                    *nmi_pending = 1;
                }
                *nmi_last = level;
            }
            if (++k == 3u) k = 0;
        }
        i++;
    }
}

void ppu_step_cycles(ppu_t *ppu, uint32_t cycles) {
    ppu_run(ppu, cycles, NULL, NULL);
}

void ppu_step_sampled(ppu_t *ppu, uint32_t cycles, uint8_t *nmi_last, int *nmi_pending) {
    ppu_run(ppu, cycles, nmi_last, nmi_pending);
}

/* Bir sonraki "CPU'ya asenkron gorunebilen" PPU olayinin, olayi icerisinde yurutecek
 * dot adimi dahil kac dot sonra oldugunu dondurur (>= 1). Olaylar (konum bazli, ppu
 * durumundan bagimsiz, bilincli olarak GENIS bir ust-kume):
 *   - mapper A12 saati adaylari: scanline 0..239 ve 261, cycle 3 ve 258
 *   - VBL bayragi: scanline 241, cycle 1 adimi (adim sonrasi cycle=2)
 * Tekli-frame tek-dot atlamasi (odd frame) en fazla 1 dot sapma yaratir; cagiran
 * taraf bir CPU cycle'lik (3 dot) guvenlik payi birakir. */
int ppu_dots_to_next_event(const ppu_t *ppu)
{
    const int s = ppu->scanline;
    const int c = ppu->cycle;

    if (s < 240 || s == 261) {
        if (c <= 3)   return 3 - c + 1;
        if (c <= 258) return 258 - c + 1;
    }
    if (s == 241 && c <= 1) return 1 - c + 1;

    int to_line_start = 341 - c;        /* sonraki satirin cycle 0'ina kadar adim */
    for (int n = 1; n <= 262; n++) {
        const int s2 = (s + n) % 262;
        if (s2 < 240 || s2 == 261) return to_line_start + 3 + 1;
        if (s2 == 241)             return to_line_start + 1 + 1;
        to_line_start += 341;
    }
    return 1;
}

void ppu_draw(const ppu_t *ppu) {
    (void)ppu;
#if EXP_FAST_BLIT
    if (s_blit_mode == 1) {
        const unsigned w = video_width();
        uint16_t *fb = (uint16_t *)video_framebuffer();
        const unsigned x_off = w - 256 - 20, y_off = 40;   /* x_off cift -> 4 bayt hizali */

        for (unsigned y = 0; y < 240; y++) {
            uint32_t *dst = (uint32_t *)(fb + (y_off + y) * w + x_off);
            const uint32_t *src = (const uint32_t *)&screen_buffer[y * 256];
            for (int k = 0; k < 128; k += 4) {
                dst[k]     = src[k];
                dst[k + 1] = src[k + 1];
                dst[k + 2] = src[k + 2];
                dst[k + 3] = src[k + 3];
            }
        }
        return;
    }

    /* Yedek yol (dikey monitor veya yerlesim uyusmazligi): 16 biti RGB555'ten genislet. */
    {
        int x_off = video_width() - 256 - 20, y_off = 40;
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 256; x++) {
                uint16_t v = screen_buffer[y * 256 + x];
                int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
                video_draw_pixel(x_off + x, y_off + y,
                                 rgb((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)));
            }
        }
    }
#else
    int x_off = video_width() - 256 - 20, y_off = 40;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 256; x++) video_draw_pixel(x_off + x, y_off + y, screen_buffer[y * 256 + x]);
    }
#endif
}