#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t lo, hi;
    uint16_t alo, ahi;
    uint8_t  nt;
    uint8_t  pal, praw;
    uint8_t  plo, phi;
    uint16_t v;
} ppu_bgstate_t;

typedef struct {
    uint8_t lo[8], hi[8];
    uint8_t attr[8];
    uint8_t cnt[8];
    uint8_t halted[8];
} ppu_spstate_t;

typedef struct {
    int scanline;
    int cycle;
    int frame;

    uint8_t reg_ctrl;
    uint8_t reg_mask;
    uint8_t reg_status;

    uint8_t open_bus;
    uint32_t open_bus_decay_timer;

    uint16_t v;
    uint16_t t;
    uint8_t  x;
    uint8_t  w;

    uint8_t vram_read_buffer;
    uint8_t oam_addr;
    uint8_t oam[256];
    uint8_t vram[0x4000];
    uint8_t palette_ram[32];
    uint8_t mirroring;

    bool vbl_suppress;

    uint8_t odd_skip_rendering;
    uint8_t odd_skip_latched;

    uint8_t sec_oam[32];
    uint8_t oam_buffer;
    uint8_t eval_phase;
    uint8_t eval_ptr;
    uint8_t eval_found;
    uint8_t eval_copy_left;
    uint8_t eval_ovf_left;
    uint8_t spr0_sec;
    uint8_t spr0_line;
    uint8_t spr_valid[8];
    uint8_t spr_x[8];
    uint8_t spr_attr[8];
    uint8_t spr_lo[8];
    uint8_t spr_hi[8];
    int     spr0_hit_cycle;
    int     ovf_set_cycle;

    uint8_t oam2_addr;
    uint8_t oam2_frozen;
    uint8_t fetch_byte[4];

    uint8_t render_age;

    uint8_t render_prev;
    uint8_t oam_corrupt_pending;
    uint8_t oam_corrupt_row;

    uint8_t  bg_ev_count;
    uint8_t  bg_ev_kind[4];
    uint8_t  bg_ev_tile[4];
    uint8_t  bg_ev_val[4];
    uint16_t bg_ev_v[4];

    ppu_bgstate_t bgs;
    ppu_spstate_t sps;
    uint8_t  bgs_start_valid;
    uint8_t  bgs_run_valid;
    uint8_t  bgs_pf_ok;
    uint8_t  bgs_on;
    int16_t  bgs_run_dot;
    uint8_t  sps_irregular;
    uint8_t  line_rendered;
    uint8_t  pre_pf;
    uint8_t  rt_count;
    uint8_t  rt_mask0;
    int16_t  rt_dot[6];
    uint8_t  rt_mask[6];
    uint8_t  rt_pend;
    int16_t  rt_pend_dot;
    uint8_t  rt_pend_new, rt_pend_old;

    uint8_t  line_latched;
    uint16_t line_v;
    uint8_t  line_x;
    uint8_t  line_bg_hi;
} ppu_t;

void ppu_init(ppu_t *ppu);
void ppu_reset(ppu_t *ppu);
void ppu_step_cycles(ppu_t *ppu, uint32_t cycles);
void ppu_step_sampled(ppu_t *ppu, uint32_t cycles, uint8_t *nmi_last, int *nmi_pending);
int ppu_dots_to_next_event(const ppu_t *ppu);
void ppu_draw(const ppu_t *ppu);
uint8_t ppu_cpu_read(ppu_t *ppu, uint16_t addr);
void ppu_cpu_write(ppu_t *ppu, uint16_t addr, uint8_t val);

int ppu_nmi_line(const ppu_t *ppu);

uint16_t ppu_core_vram_mask(uint16_t a, uint8_t mirroring);
uint8_t  ppu_core_palette_mirror_index(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif