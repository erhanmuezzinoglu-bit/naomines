#include "apu.h"
#include "exp_config.h"
#include <string.h>
#include <stdint.h>
#include "emu_driver.h"

#define APU_CPU_CLOCK_HZ   1789773u
#define APU_OUTPUT_HZ      44100u

static const uint8_t length_table[] = {
    10, 254, 20, 2, 40, 4, 80, 6,
    160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
    192, 24, 72, 26, 16, 28, 32, 30
};

static const uint8_t duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}
};

static const uint8_t triangle_sequence[32] = {
    15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15
};

static const uint16_t noise_timer_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160,
    202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const uint16_t dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106, 84, 72, 54
};

int apu_irq_line(const apu_t *apu)
{
    if (!apu) return 0;

    if (apu->dmc.irq_pending) {
        return 1;
    }

    if (apu->frame_counter_irq &&
        !(apu->reg_4017_frame_counter & 0x40)) {
        return 1;
    }

    return 0;
}

#if EXP_DIAG_PROF
int g_apu_shadow_active = 0;   /* olcum kipi: golge APU'nun IRQ hatti gercek CPU'ya yansimaz */
#endif

static void apu_update_irq_line(apu_t *apu)
{
#if EXP_DIAG_PROF
    if (g_apu_shadow_active) return;
#endif
    if (apu_irq_line(apu)) {
        emu_driver_request_irq();
    } else {
        emu_driver_clear_irq();
    }
}

static void dmc_schedule_load(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    d->enable_reload_gate = 0;

    d->dma_request = 1;
    d->dma_kind = APU_DMC_DMA_LOAD;
    d->dma_delay_parity = 0;

    if ((apu->total_cycles & 1) == 0) {
        d->dma_delay = 3;
    } else {
        d->dma_delay = 2;
    }
}

static void dmc_schedule_reload(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    int gated_reload =
        d->enable_delay > 0 &&
        d->enabled &&
        d->current_length > 0 &&
        !d->buffer_full &&
        !d->implicit_abort_pending;

    d->enable_reload_gate = gated_reload ? 1 : 0;

    d->dma_request = 1;
    d->dma_kind = APU_DMC_DMA_RELOAD;

    if (d->implicit_abort_pending) {
        d->dma_delay_parity =
            0xA0u |
            (uint8_t)((apu->total_cycles & 1) ? 1 : 0);
        d->implicit_abort_pending = 0;
    } else {
        d->dma_delay_parity = 0;
    }

    if ((apu->total_cycles & 1) == 0) {
        d->dma_delay = 0;
    } else {
        d->dma_delay = 1;
    }

    if (gated_reload) {
        d->dma_delay = 0;
    }
}

// ------------------------------------------------------------
// PULSE
// ------------------------------------------------------------

static uint16_t pulse_target_period(
    const pulse_t *p,
    int is_pulse1)
{
    uint16_t shift = p->timer_period >> p->sweep_shift;

    if (p->sweep_negate) {
        return is_pulse1
            ? p->timer_period - shift - 1
            : p->timer_period - shift;
    }

    return p->timer_period + shift;
}

static int pulse_is_muted(const pulse_t *p, int is_pulse1)
{
    if (p->timer_period < 8) return 1;
    if (pulse_target_period(p, is_pulse1) > 0x7FF) return 1;
    return 0;
}

static void pulse_clock_timer(pulse_t *p)
{
    if (p->timer == 0) {
        p->timer = p->timer_period;
        p->duty_step = (p->duty_step + 1) & 7;
    } else {
        p->timer--;
    }
}

static void pulse_clock_envelope(pulse_t *p)
{
    if (p->envelope_start) {
        p->envelope_start = 0;
        p->envelope_decay = 15;
        p->envelope_divider = p->volume_or_period;
    } else {
        if (p->envelope_divider == 0) {
            p->envelope_divider = p->volume_or_period;
            if (p->envelope_decay > 0) {
                p->envelope_decay--;
            } else if (p->length_halt) {
                p->envelope_decay = 15;
            }
        } else {
            p->envelope_divider--;
        }
    }
}

static void pulse_clock_length(pulse_t *p, uint8_t halt)
{
    if (!halt && p->length_counter > 0) {
        p->length_counter--;
    }
}

static void pulse_clock_sweep(pulse_t *p, int is_pulse1)
{
    uint16_t target = pulse_target_period(p, is_pulse1);

    if (p->sweep_divider == 0 &&
        p->sweep_enable &&
        p->sweep_shift > 0 &&
        target <= 0x7FF) {
        p->timer_period = target;
    }

    if (p->sweep_divider == 0 || p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = 0;
    } else {
        p->sweep_divider--;
    }
}

static uint8_t pulse_output(const pulse_t *p, int is_pulse1)
{
    if (!p->enabled ||
        p->length_counter == 0 ||
        pulse_is_muted(p, is_pulse1)) {
        return 0;
    }

    if (duty_table[p->duty][p->duty_step] == 0) {
        return 0;
    }

    return p->constant_volume
        ? p->volume_or_period
        : p->envelope_decay;
}

// ------------------------------------------------------------
// TRIANGLE
// ------------------------------------------------------------

static void triangle_clock_timer(triangle_t *t)
{
    if (t->timer == 0) {
        t->timer = t->timer_period;
        if (t->length_counter > 0 &&
            t->linear_counter > 0 &&
            t->timer_period > 2) {
            t->duty_step = (t->duty_step + 1) & 0x1F;
        }
    } else {
        t->timer--;
    }
}

static void triangle_clock_linear(triangle_t *t)
{
    if (t->linear_counter_reload_flag) {
        t->linear_counter = t->linear_counter_reload_val;
    } else if (t->linear_counter > 0) {
        t->linear_counter--;
    }

    if (!t->length_halt) {
        t->linear_counter_reload_flag = 0;
    }
}

static void triangle_clock_length(triangle_t *t, uint8_t halt)
{
    if (!halt && t->length_counter > 0) {
        t->length_counter--;
    }
}

static uint8_t triangle_output(const triangle_t *t)
{
    if (!t->enabled ||
        t->length_counter == 0 ||
        t->linear_counter == 0) {
        return 0;
    }
    return triangle_sequence[t->duty_step];
}

// ------------------------------------------------------------
// NOISE
// ------------------------------------------------------------

static void noise_clock_timer(noise_t *n)
{
    if (n->timer == 0) {
        n->timer = n->timer_period;

        uint8_t shift_amt = n->mode ? 6 : 1;
        uint16_t feedback =
            (n->shift_register & 1) ^
            ((n->shift_register >> shift_amt) & 1);

        n->shift_register >>= 1;
        n->shift_register |= (feedback << 14);
    } else {
        n->timer--;
    }
}

static void noise_clock_envelope(noise_t *n)
{
    if (n->envelope_start) {
        n->envelope_start = 0;
        n->envelope_decay = 15;
        n->envelope_divider = n->volume_or_period;
    } else {
        if (n->envelope_divider == 0) {
            n->envelope_divider = n->volume_or_period;
            if (n->envelope_decay > 0) {
                n->envelope_decay--;
            } else if (n->length_halt) {
                n->envelope_decay = 15;
            }
        } else {
            n->envelope_divider--;
        }
    }
}

static void noise_clock_length(noise_t *n, uint8_t halt)
{
    if (!halt && n->length_counter > 0) {
        n->length_counter--;
    }
}

static uint8_t noise_output(const noise_t *n)
{
    if (!n->enabled || n->length_counter == 0) return 0;
    if (n->shift_register & 1) return 0;
    return n->constant_volume ? n->volume_or_period : n->envelope_decay;
}

// ------------------------------------------------------------
// DMC
// ------------------------------------------------------------

static void dmc_process_disable_delay(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    if (d->disable_delay == 0) return;
    d->disable_delay--;
    if (d->disable_delay != 0) return;

    d->current_length = 0;
    d->implicit_abort_pending = 0;
    d->implicit_abort_active = 0;

    if (d->dma_delay_parity == DMC_DMA_RUNTIME_ACTIVE) {
        d->dma_delay_parity = DMC_DMA_RUNTIME_ABORT;
    } else if (d->dma_request) {
        d->dma_request = 0;
        d->dma_kind = APU_DMC_DMA_NONE;
        d->dma_delay = 0;
        d->dma_delay_parity = 0;
    }
}

static void dmc_process_implicit_stop_delay(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    if (d->implicit_stop_delay == 0) return;
    d->implicit_stop_delay--;
    if (d->implicit_stop_delay != 0) return;

    d->current_length = 0;
    d->implicit_abort_pending = 0;
    d->implicit_abort_active = 1;

    if ((d->dma_delay_parity & 0xFEu) ==
        DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE) {
        if (d->dma_request) {
            d->dma_request = 0;
            d->dma_kind = APU_DMC_DMA_NONE;
            d->dma_delay = 0;
            d->dma_delay_parity = 0;
        } else {
            d->dma_delay_parity = DMC_DMA_RUNTIME_IMPLICIT_ABORT;
        }
    }
    else if ((d->dma_delay_parity & 0xFEu) == 0x84u) {
        d->dma_request = 0;
        d->dma_kind = APU_DMC_DMA_NONE;
        d->dma_delay = 0;
        d->dma_delay_parity = 0;
    }
    else if (d->dma_delay_parity ==
             DMC_DMA_RUNTIME_IMPLICIT_ACTIVE) {
        d->dma_delay_parity = DMC_DMA_RUNTIME_IMPLICIT_ABORT;
    }
}

void apu_dmc_arm_implicit_abort(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    d->implicit_abort_pending = 1;
    d->implicit_abort_active = 0;
    d->implicit_stop_delay = 3;
}

static void dmc_process_loop_implicit_delay(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    if (d->loop_implicit_delay == 0) return;

    if (!d->enabled ||
        !d->loop ||
        d->sample_length != 1 ||
        d->current_length != 1 ||
        d->buffer_full) {
        d->loop_implicit_delay = 0;
        return;
    }

    if (d->dma_request) {
        d->loop_implicit_delay = 0;
        return;
    }

    d->loop_implicit_delay--;
    if (d->loop_implicit_delay != 0) return;

    dmc_schedule_reload(apu);
}

static void dmc_process_enable_delay(apu_t *apu)
{
    if (apu->dmc.enable_delay == 0) return;
    apu->dmc.enable_delay--;
}

int apu_dmc_enable_blocks_halt(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    if (!d->enable_reload_gate ||
        !d->dma_request ||
        d->dma_kind != APU_DMC_DMA_RELOAD) {
        d->enable_reload_gate = 0;
        return 0;
    }

    if (d->enable_delay == 0) {
        d->enable_reload_gate = 0;
        return 0;
    }

    d->dma_delay_parity ^= 1;
    return 1;
}

static void dmc_clock_timer(apu_t *apu)
{
    dmc_t *d = &apu->dmc;

    if (d->timer == 0) {
        d->timer = d->timer_period;
        d->timer_reloaded_this_cycle = 1;

        if (d->bit_count > 0) {
            if (d->shift_register & 1) {
                if (d->value <= 125) d->value += 2;
            } else {
                if (d->value >= 2) d->value -= 2;
            }
            d->shift_register >>= 1;
            d->bit_count--;
        }

        if (d->bit_count == 0) {
            d->bit_count = 8;

            if (d->buffer_full) {
                d->shift_register = d->sample_buffer;
                d->buffer_full = false;

                if (d->current_length > 0 ||
                    d->implicit_abort_pending) {
                    dmc_schedule_reload(apu);
                }
            } else {
                if (d->current_length == 0 &&
                    d->enabled &&
                    !d->implicit_abort_active &&
                    !d->implicit_abort_pending) {
                    if (d->loop) {
                        /* donanımda son bayt ile yeniden başlatılır */
                    } else if (d->irq_enable) {
                        d->irq_pending = 1;
                        apu_update_irq_line(apu);
                    }
                }
            }
        }
    } else {
        d->timer--;
    }
}

// ------------------------------------------------------------
// FRAME SEQUENCER HELPERS
// ------------------------------------------------------------

static void apu_quarter_frame(apu_t *apu)
{
    pulse_clock_envelope(&apu->p1);
    pulse_clock_envelope(&apu->p2);
    noise_clock_envelope(&apu->noise);
    triangle_clock_linear(&apu->tri);
}

static void apu_half_frame(apu_t *apu)
{
    apu_quarter_frame(apu);

    pulse_clock_sweep(&apu->p1, 1);
    pulse_clock_sweep(&apu->p2, 0);

    apu->pending_length_halt_mask =
        (apu->p1.length_halt    ? 0x01 : 0) |
        (apu->p2.length_halt    ? 0x02 : 0) |
        (apu->tri.length_halt   ? 0x04 : 0) |
        (apu->noise.length_halt ? 0x08 : 0);

    apu->pending_length_skip_mask = 0;
    apu->pending_length_clock = 1;
}

static void apu_apply_pending_length_clock(apu_t *apu)
{
    if (!apu->pending_length_clock) return;

    uint8_t halt_mask = apu->pending_length_halt_mask;
    uint8_t skip_mask = apu->pending_length_skip_mask;

    apu->pending_length_clock = 0;
    apu->pending_length_halt_mask = 0;
    apu->pending_length_skip_mask = 0;

    if (!(skip_mask & 0x01)) pulse_clock_length(&apu->p1, halt_mask & 0x01);
    if (!(skip_mask & 0x02)) pulse_clock_length(&apu->p2, halt_mask & 0x02);
    if (!(skip_mask & 0x08)) noise_clock_length(&apu->noise, halt_mask & 0x08);
    if (!(skip_mask & 0x04)) triangle_clock_length(&apu->tri, halt_mask & 0x04);
}

// ------------------------------------------------------------
// MIXER LUT
// ------------------------------------------------------------

#define APU_MIX_TRI_DIM    16
#define APU_MIX_NOISE_DIM  16
#define APU_MIX_DMC_DIM   128

static float s_pulse_out_lut[31];
static float s_tnd_out_lut[APU_MIX_TRI_DIM]
                          [APU_MIX_NOISE_DIM]
                          [APU_MIX_DMC_DIM];
static int   s_mix_lut_ready = 0;

static void apu_mix_lut_init(void)
{
    if (s_mix_lut_ready) return;
    s_mix_lut_ready = 1;

    s_pulse_out_lut[0] = 0.0f;
    for (int i = 1; i <= 30; i++) {
        s_pulse_out_lut[i] =
            95.88f / (8128.0f / (float)i + 100.0f);
    }

    for (int tri = 0; tri < APU_MIX_TRI_DIM; tri++) {
        for (int noise = 0; noise < APU_MIX_NOISE_DIM; noise++) {
            for (int dmc = 0; dmc < APU_MIX_DMC_DIM; dmc++) {
                float denom =
                    (float)tri / 8227.0f +
                    (float)noise / 12241.0f +
                    (float)dmc / 22638.0f;

                float out = 0.0f;
                if (denom > 0.0f) {
                    out = 159.79f / (1.0f / denom + 100.0f);
                }
                s_tnd_out_lut[tri][noise][dmc] = out;
            }
        }
    }
}

static int16_t apu_mix(
    uint8_t p1,
    uint8_t p2,
    uint8_t tri,
    uint8_t noise,
    uint8_t dmc)
{
    float pulse_out = s_pulse_out_lut[p1 + p2];
    float tnd_out   = s_tnd_out_lut[tri][noise][dmc];

    return (int16_t)((pulse_out + tnd_out) * 30000.0f);
}

// ------------------------------------------------------------
// INIT / RESET
// ------------------------------------------------------------

void apu_init(apu_t *apu)
{
    apu_mix_lut_init();

    memset(apu, 0, sizeof(*apu));

    apu->noise.shift_register = 1;

    apu->dmc.timer_period = dmc_rate_table[0] - 1;
    apu->dmc.timer = apu->dmc.timer_period;
    apu->dmc.bit_count = 8;
    apu->dmc.shift_register = 0;

    apu->dmc.sample_address = 0xC000;
    apu->dmc.current_address = 0xC000;
    apu->dmc.sample_length = 1;

    apu->total_cycles = 0;
    apu->reg_4017_frame_counter = 0x00;
    apu->frame_counter_mode = 0;
    apu->frame_seq_count = 0;
    apu->pending_length_clock = 0;
    apu->frame_counter_reset_pending = 0;

    apu->dmc.disable_delay = 0;
    apu->dmc.implicit_stop_delay = 0;
    apu->dmc.enable_delay = 0;
    apu->dmc.enable_reload_gate = 0;
    apu->dmc.implicit_abort_pending = 0;
    apu->dmc.implicit_abort_active = 0;
    apu->dmc.loop_implicit_delay = 0;
    apu->dmc.timer_reloaded_this_cycle = 0;

    apu->sample_count = 0;
    apu->sample_accumulator = 0;
}

void apu_reset(apu_t *apu)
{
    apu->dmc.enable_delay = 0;
    apu->dmc.enable_reload_gate = 0;

    apu->p1.enabled = 0;
    apu->p2.enabled = 0;
    apu->tri.enabled = 0;
    apu->noise.enabled = 0;
    apu->dmc.enabled = 0;

    apu->p1.length_counter = 0;
    apu->p2.length_counter = 0;
    apu->tri.length_counter = 0;
    apu->noise.length_counter = 0;
    apu->dmc.current_length = 0;

    apu->dmc.dma_request = 0;
    apu->dmc.dma_kind = APU_DMC_DMA_NONE;
    apu->dmc.dma_delay = 0;
    apu->dmc.dma_delay_parity = 0;

    apu->dmc.buffer_full = false;

    apu->dmc.timer = apu->dmc.timer_period;
    apu->dmc.bit_count = 8;
    apu->dmc.shift_register = 0;

    apu->noise.shift_register = 1;
    apu->dmc.irq_pending = 0;

    apu->frame_counter_irq = 0;
    apu->pending_irq_clear = 0;
    apu->pending_length_clock = 0;
    apu->pending_length_skip_mask = 0;
    apu->pending_length_halt_mask = 0;
    apu->pending_frame_irq_reassert = 0;
    apu->frame_counter_reset_pending = 0;
    apu->frame_counter_mode = (apu->reg_4017_frame_counter >> 7) & 1;

    apu->dmc.disable_delay = 0;
    apu->dmc.implicit_stop_delay = 0;
    apu->dmc.implicit_abort_pending = 0;
    apu->dmc.implicit_abort_active = 0;
    apu->dmc.loop_implicit_delay = 0;
    apu->dmc.timer_reloaded_this_cycle = 0;

    apu->sample_count = 0;

    apu->reset_delay = 0;
    apu->frame_seq_count = 0;

    if (apu->frame_counter_mode == 1) {
        apu_half_frame(apu);
    }

    apu_update_irq_line(apu);
}

// ------------------------------------------------------------
// STEP
// ------------------------------------------------------------

/* Tek APU cycle'i: eski apu_step() dongusunun govdesi, davranisi AYNIDIR. */
static void apu_cycle(apu_t *apu)
{

        apu->dmc.timer_reloaded_this_cycle = 0;
        apu->total_cycles++;

        if (apu->pending_irq_clear &&
            (apu->total_cycles & 1u) == 0) {
            apu->pending_irq_clear = 0;
            apu->frame_counter_irq = 0;
            apu_update_irq_line(apu);
        }

        if (apu->pending_frame_irq_reassert) {
            apu->pending_frame_irq_reassert = 0;

            if (apu->frame_counter_mode == 0 &&
                !(apu->reg_4017_frame_counter & 0x40)) {
                apu->frame_counter_irq = 1;
                apu_update_irq_line(apu);
            } else if (apu->frame_counter_mode == 0 &&
                       (apu->reg_4017_frame_counter & 0x40)) {
                apu->frame_counter_irq = 0;
                apu_update_irq_line(apu);
            }
        }

        apu_apply_pending_length_clock(apu);

        dmc_process_implicit_stop_delay(apu);
        dmc_process_disable_delay(apu);
        dmc_process_enable_delay(apu);
        dmc_process_loop_implicit_delay(apu);

        if (apu->reset_delay > 0) {
            apu->reset_delay--;
            if (apu->reset_delay == 0 &&
                apu->frame_counter_reset_pending) {
                apu->frame_counter_reset_pending = 0;
                apu->frame_seq_count = 0;
                if (apu->frame_counter_mode == 1) {
                    apu_half_frame(apu);
                    apu_apply_pending_length_clock(apu);
                }
            }
        }

        if ((apu->total_cycles & 1) == 0) {
            pulse_clock_timer(&apu->p1);
            pulse_clock_timer(&apu->p2);
        }

        dmc_clock_timer(apu);
        triangle_clock_timer(&apu->tri);
        noise_clock_timer(&apu->noise);

        apu->frame_seq_count++;

        if (apu->frame_counter_mode == 0) {
            if (apu->frame_seq_count == 7457) {
                apu_quarter_frame(apu);
            } else if (apu->frame_seq_count == 14913) {
                apu_half_frame(apu);
            } else if (apu->frame_seq_count == 22371) {
                apu_quarter_frame(apu);
            } else if (apu->frame_seq_count == 29829) {
                apu_half_frame(apu);

                apu->frame_counter_irq = 1;
                apu_update_irq_line(apu);

            } else if (apu->frame_seq_count == 29830) {
                if (!(apu->reg_4017_frame_counter & 0x40)) {
                    apu->frame_counter_irq = 1;
                    apu_update_irq_line(apu);
                }
                apu->pending_frame_irq_reassert = 1;
                apu->frame_seq_count = 0;
            }
        } else {
            if (apu->frame_seq_count == 7457) {
                apu_quarter_frame(apu);
            } else if (apu->frame_seq_count == 14913) {
                apu_half_frame(apu);
            } else if (apu->frame_seq_count == 22371) {
                apu_quarter_frame(apu);
            } else if (apu->frame_seq_count == 37281) {
                apu_half_frame(apu);
            } else if (apu->frame_seq_count == 37282) {
                apu->frame_seq_count = 0;
            }
        }

        apu->sample_accumulator += APU_OUTPUT_HZ;

        if (apu->sample_accumulator >= APU_CPU_CLOCK_HZ) {
            apu->sample_accumulator -= APU_CPU_CLOCK_HZ;

            if (apu->sample_count < 8192) {
                apu->sample_buffer[apu->sample_count++] = apu_mix(
                    pulse_output(&apu->p1, 1),
                    pulse_output(&apu->p2, 0),
                    triangle_output(&apu->tri),
                    noise_output(&apu->noise),
                    apu->dmc.value
                );
            }
        }
}

/* ------------------------------------------------------------
 * TOPLU ILERLETME
 *
 * "Sessiz" durum: bekleyen hicbir gecikme/bayrak yok. Bu durumda bir cycle'in
 * yaptigi tek sey zamanlayici ilerletmeleri, frame/ornek sayaclari ve (olay
 * cycle'larinda) sequencer/DMC/ornek uretimidir. Olay icermeyen cycle'lar kapali
 * formda toplu ilerletilir; olay cycle'i orijinal govdeyle (apu_cycle) calistirilir.
 * ------------------------------------------------------------ */
static inline int apu_quiescent(const apu_t *a)
{
    return !(a->pending_irq_clear |
             a->pending_frame_irq_reassert |
             a->pending_length_clock |
             a->dmc.implicit_stop_delay |
             a->dmc.disable_delay |
             a->dmc.enable_delay |
             a->dmc.loop_implicit_delay |
             (a->reset_delay != 0));
}

/* Siradaki frame-sequencer olayina kalan cycle sayisi (olay, o cycle'da calisir; >=1).
 * Olay yoksa UINT32_MAX. */
static uint32_t apu_frame_event_distance(const apu_t *a)
{
    static const uint32_t ev0[5] = { 7457u, 14913u, 22371u, 29829u, 29830u };
    static const uint32_t ev1[5] = { 7457u, 14913u, 22371u, 37281u, 37282u };
    const uint32_t *ev = (a->frame_counter_mode == 0) ? ev0 : ev1;
    const uint32_t c = a->frame_seq_count;

    for (int k = 0; k < 5; k++) {
        if (ev[k] > c) return ev[k] - c;
    }
    return 0xFFFFFFFFu;
}

/* m zamanlayici adimi: pulse/triangle/noise ortak kapali form. Donus: reload sayisi. */
static inline uint32_t timer_bulk(uint16_t *timer, uint16_t period, uint32_t m)
{
    if (m <= *timer) {
        *timer = (uint16_t)(*timer - m);
        return 0;
    }
    m -= (uint32_t)*timer + 1u;
    const uint32_t p1 = (uint32_t)period + 1u;
    /* SH-4'te degiskenle bolme yazilim yordami (pahali); ornek araligi (~40 cycle) icinde reload sayisi kucuktur:
     * kucuk bolumler (en kisa gurultu periyodunda ornek basina ~10 reload dahil) cikarma ile (sonuc birebir ayni). */
    if (m < p1) {
        *timer = (uint16_t)(period - m);
        return 1u;
    }
    if (m < 16u * p1) {
        uint32_t r = 1u;
        do { m -= p1; r++; } while (m >= p1);
        *timer = (uint16_t)(period - m);
        return r;
    }
    *timer = (uint16_t)(period - (m % p1));
    return 1u + m / p1;
}

/* dmc_clock_timer'in timer == 0 ve bit_count >= 2 dalinin bit/cikis kismi (bayt sonu degil); timer'i ve bayragi
 * toplu ilerletme (timer_bulk) zaten yeniden yuklemistir. */
static inline void dmc_simple_reload(dmc_t *d)
{
    if (d->shift_register & 1) {
        if (d->value <= 125) d->value += 2;
    } else {
        if (d->value >= 2) d->value -= 2;
    }
    d->shift_register >>= 1;
    d->bit_count--;
}

static void apu_bulk_advance(apu_t *apu, uint32_t s)
{
    const uint64_t t0 = apu->total_cycles;
    const uint32_t evens = (uint32_t)(((t0 + s) >> 1) - (t0 >> 1));

    /* pulse: yalniz cift cycle'larda */
    {
        uint32_t r = timer_bulk(&apu->p1.timer, apu->p1.timer_period, evens);
        apu->p1.duty_step = (uint8_t)((apu->p1.duty_step + r) & 7u);
        r = timer_bulk(&apu->p2.timer, apu->p2.timer_period, evens);
        apu->p2.duty_step = (uint8_t)((apu->p2.duty_step + r) & 7u);
    }

    /* triangle: duty yalniz sart saglaniyorsa ilerler (sart bulk boyunca sabit) */
    {
        triangle_t *t = &apu->tri;
        uint32_t r = timer_bulk(&t->timer, t->timer_period, s);
        if (t->length_counter > 0 && t->linear_counter > 0 && t->timer_period > 2) {
            t->duty_step = (uint8_t)((t->duty_step + r) & 0x1Fu);
        }
    }

    /* noise: her reload'da bir LFSR adimi */
    {
        noise_t *n = &apu->noise;
        uint32_t r = timer_bulk(&n->timer, n->timer_period, s);
        /* LFSR'yi r adim birden ilerlet (tek adimlik dongunun birebir sonucu). Yazmac 15 bittir (baslangic 1, tek
         * degistiren adim bit 14'e yazar). Bit i = x[n+i] dizisiyle: tek adim x[n+15] = x[n] ^ x[n+a] (a = 1 ya da 6).
         * k adimda yeni bitler x[n+15+j] = x[n+j] ^ x[n+j+a] (j < k), j + a <= 14 oldugu surece hepsi eski durumdan gelir:
         * k <= 15 - a (mod 0: 14, mod 1: 9). Daha uzun r parcalara bolunur. */
        if (r) {
            const unsigned a = n->mode ? 6u : 1u;
            const unsigned kmax = 15u - a;
            unsigned sr = n->shift_register;
            while (r) {
                const unsigned k = r < kmax ? r : kmax;
                const unsigned nb = (sr ^ (sr >> a)) & ((1u << k) - 1u);
                sr = (sr >> k) | (nb << (15u - k));
                r -= k;
            }
            n->shift_register = (uint16_t)sr;
        }
    }

    /* DMC: cagiran araligi siradaki BAYT SONU yeniden yuklemesinden once bitirir; aradaki yeniden yuklemeler (bit_count
     * >= 2) yalniz cikis/kaydirici/sayaci degistirir (dmc_simple_reload). Ornek araligi (<= 41 cycle) en kisa DMC
     * periyodundan (54) kisa oldugu icin adim basina en fazla 1 yeniden yukleme olur (genel sayi da dogru islenir).
     * Yeniden yukleme adimin son cycle'indaysa bayrak 1 (yuklemeden sonra timer = period; n > 0 iken timer == period
     * yalniz son cycle yuklemeyse). Ornek, adimin son cycle'indan SONRA karistirildigi icin DMC guncellemesini gorur
     * (apu_cycle sirasi: dmc_clock_timer, sonra ornek). */
    {
        dmc_t *const d = &apu->dmc;
        uint32_t dn = timer_bulk(&d->timer, d->timer_period, s);
        d->timer_reloaded_this_cycle = (dn != 0u && d->timer == d->timer_period);
        while (dn) { dmc_simple_reload(d); dn--; }
    }

    apu->frame_seq_count += s;
    apu->total_cycles = t0 + s;
    apu->sample_accumulator += APU_OUTPUT_HZ * s;
}

/* Siradaki "CPU'ya gorunebilen" APU olayina kalan cycle sayisi (>=1): sessiz degilse 1;
 * DMC sayac reload'i (DMA istegi/IRQ olusturabilir) ve frame-sequencer olaylari (IRQ). */
int apu_cycles_to_next_event(const apu_t *apu)
{
    if (!apu_quiescent(apu)) {
        /* $4015 okumasi pending_irq_clear kurar (siradaki cift cycle'da frame IRQ bayragi temizlenir). Bayrak zaten 0 ise
         * bu temizleme CPU'ya gorunmez (IRQ hatti degismez). Bayragi bu arada 1 yapabilecek tek sey frame-sequencer IRQ
         * olayidir (asagida olay mesafesine zaten dahil) ya da pending_frame_irq_reassert (sessiz degil -> 1). Diger
         * bekleyenler yoksa siradaki gorunur olay, sessiz durumdaki gibi hesaplanir; aradaki cycle'lari apu_step birebir
         * isler (temizleme kendi cift cycle'inda yapilir). Ses surucusunun $4015'i sik okudugu oyunlarda (karede yuzlerce)
         * okuma basina 1-2 zorunlu APU senkronu kalkar. */
        const int only_clear = apu->pending_irq_clear && !apu->frame_counter_irq &&
            !(apu->pending_frame_irq_reassert | apu->pending_length_clock |
              apu->dmc.implicit_stop_delay | apu->dmc.disable_delay | apu->dmc.enable_delay | apu->dmc.loop_implicit_delay |
              (apu->reset_delay != 0));
        if (!only_clear) return 1;
    }

    /* DMC: CPU'ya gorunen etki (DMA istegi / IRQ) YALNIZ bit_count'u 0'a indiren zamanlayici yeniden yuklemesinde
     * olusur (dmc_clock_timer). Diger yeniden yuklemeler yalnizca cikis seviyesini, kaydiriciyi ve sayaclari
     * degistirir (CPU gormez; $4010 yazimi senkrondan sonra okunur) ve apu_step icinde sirayla islenir. Bu yuzden olay
     * mesafesi, siradaki "bayt sonu" yeniden yuklemesine kadardir:
     *   (timer + 1) + (bit_count - 1) * (timer_period + 1)
     * (eskiden her yeniden yukleme olay sayiliyordu: en hizli oranda karede ~550 zorunlu senkron). */
    const dmc_t *const d = &apu->dmc;
    uint32_t e = (uint32_t)d->timer + 1u;
    if (d->bit_count > 1u) {
        e += ((uint32_t)d->bit_count - 1u) * ((uint32_t)d->timer_period + 1u);
    }
    /* DMC bosta (okunacak bayt yok, DMA istegi/iptal yok): bayt sonu yeniden yuklemesi CPU'ya gorunmez. dmc_clock_timer'a gore
     * bayt sonunda yalniz iki gorunur etki var: (1) tampon doluyken current_length > 0 ya da iptal bekliyorsa yeni DMA istegi,
     * (2) tampon bosken (current_length == 0, etkin, iptal yok, dongu yok, irq_enable) DMC IRQ bayragi. (1) burada imkansiz;
     * (2) bayrak zaten kuruluysa degisiklik yapmaz. Bu alanlari degistirebilen tek sey CPU yazimi ($4010/$4015, zorunlu senkron)
     * ve DMA'dir (burada yok); tampon doluysa ilk bayt sonu onu bosaltir, IRQ en erken ikinci bayt sonundadir. Boylece hizli DMC
     * oraninda ama sessizken (bayt basina bir zorunlu senkron) olay sayisi duser. */
    if (!d->dma_request && d->current_length == 0 && !d->implicit_abort_pending && !d->implicit_abort_active) {
        const int irq_possible = d->enabled && !d->loop && d->irq_enable && !d->irq_pending;
        if (!irq_possible) e = 0x00FFFFFFu;
        else if (d->buffer_full) e += 8u * ((uint32_t)d->timer_period + 1u);
    }
    uint32_t f = apu_frame_event_distance(apu);
    if (f < e) e = f;
    if (e > 0x00FFFFFFu) e = 0x00FFFFFFu;
    return (int)e;
}

static inline void apu_emit_sample(apu_t *apu)
{
    apu->sample_accumulator -= APU_CPU_CLOCK_HZ;
    if (apu->sample_count < 8192) {
        apu->sample_buffer[apu->sample_count++] = apu_mix(
            pulse_output(&apu->p1, 1),
            pulse_output(&apu->p2, 0),
            triangle_output(&apu->tri),
            noise_output(&apu->noise),
            apu->dmc.value
        );
    }
}

void apu_step(apu_t *apu, int cpu_cycles)
{
    int remaining = cpu_cycles;

    while (remaining > 0) {
        /* $4015 okumasindan kalan, CPU'ya gorunmez frame IRQ temizlemesi (bayrak zaten 0, baska bekleyen yok): apu_cycle
         * bunu siradaki cift cycle'da (ce = 1 ya da 2) yapar: frame_counter_irq = 0 (zaten 0) ve IRQ hattini yeniden
         * hesaplar (bayraklar degismedigi icin degismez: g_irq_pending her zaman APU/mapper kaynaklarinin VEYA'sidir).
         * O cycle'dan once bayragi 1 yapabilecek bir frame olayi yoksa (olay ce'de ise once temizleme, sonra olay calisir:
         * temizlemeyi atlamakla ayni) ve adim ce'yi kapsiyorsa, bekleyen bayrak simdi dusurulur; aradaki cycle'lar sessiz
         * yoldan (toplu) islenir. Boylece okuma basina 1-2 apu_cycle ve aralik bolunmesi kalkar. */
        if (apu->pending_irq_clear && !apu->frame_counter_irq &&
            !(apu->pending_frame_irq_reassert | apu->pending_length_clock |
              apu->dmc.implicit_stop_delay | apu->dmc.disable_delay | apu->dmc.enable_delay | apu->dmc.loop_implicit_delay |
              (apu->reset_delay != 0))) {
            const uint32_t ce = ((apu->total_cycles + 1u) & 1u) ? 2u : 1u;
            if (ce <= (uint32_t)remaining && ce <= apu_frame_event_distance(apu)) apu->pending_irq_clear = 0;
        }
        if (remaining > 1 && apu_quiescent(apu)) {
            /* Sessiz durumda olay cycle'lari: frame-sequencer olaylari ve DMC yeniden yuklemeleri. Olay icermeyen bastaki
             * cycle'lar (span) toplu ilerletilir; aradaki ornekler araligin icinde uretilir (apu_cycle ile ayni sira:
             * ornek o cycle'in zamanlayici guncellemelerinden SONRA karistirilir).
             * DMC bosta (kaydirici 0, cikis < 2, bit_count >= 1): bayt sonu olmayan yeniden yuklemeler cikisi degistirmez
             * (0 biti: value >= 2 degil -> ayni; kaydirici 0 kalir); yalniz timer/bit_count/bayrak degisir. Bu yuzden
             * span siradaki BAYT SONU yeniden yuklemesine kadar uzar ve DMC sayaci span sonunda tek seferde ilerletilir.
             * DMC bosta degilse span siradaki yeniden yuklemeden once biter (eski davranis). */
            /* Sessiz durumda olay cycle'lari: frame-sequencer olaylari ve DMC'nin BAYT SONU yeniden yuklemesi (DMA istegi /
             * IRQ / kaydirici yuklemesi). Aradaki DMC yeniden yuklemeleri (bit_count >= 2) toplu ilerletmenin icinde
             * islenir (apu_bulk_advance). Olay icermeyen bastaki cycle'lar (span) toplu ilerletilir; aradaki ornekler
             * araligin icinde uretilir (apu_cycle ile ayni sira: ornek o cycle'in guncellemelerinden SONRA). */
            const dmc_t *const dm = &apu->dmc;
            uint32_t ev = (uint32_t)dm->timer + 1u;                     /* DMC yeniden yukleme cycle'i (1 tabanli) */
            if (dm->bit_count >= 2u) ev += ((uint32_t)dm->bit_count - 1u) * ((uint32_t)dm->timer_period + 1u);
            const uint32_t f = apu_frame_event_distance(apu);
            if (f < ev) ev = f;

            uint32_t span = ev - 1u;                    /* olay icermeyen bastaki cycle sayisi */
            if (span > (uint32_t)remaining) span = (uint32_t)remaining;
            remaining -= (int)span;
            while (span > 0u) {
                /* siradaki ornegi ureten cycle'in indeksi (1 tabanli; sabite bolme: carpmaya donusur) */
                const uint32_t smp = (APU_CPU_CLOCK_HZ - apu->sample_accumulator + APU_OUTPUT_HZ - 1u) / APU_OUTPUT_HZ;
                if (smp > span) {
                    apu_bulk_advance(apu, span);
                    break;
                }
                apu_bulk_advance(apu, smp);            /* ornek cycle'i dahil */
                apu_emit_sample(apu);
                span -= smp;
            }
            if (remaining > 0) {
                apu_cycle(apu);                        /* olay cycle'i ya da kalan son cycle */
                remaining--;
            }
        } else {
            apu_cycle(apu);
            remaining--;
        }
    }
}

// ------------------------------------------------------------
// CPU READ / WRITE
// ------------------------------------------------------------

uint8_t apu_cpu_read(
    apu_t *apu,
    uint16_t addr,
    uint8_t open_bus_data)
{
    if (addr == 0x4015) {
        uint8_t res = 0;

        if (apu->p1.length_counter > 0)    res |= 0x01;
        if (apu->p2.length_counter > 0)    res |= 0x02;
        if (apu->tri.length_counter > 0)   res |= 0x04;
        if (apu->noise.length_counter > 0) res |= 0x08;
        if (apu->dmc.current_length > 0)   res |= 0x10;
        if (apu->frame_counter_irq)        res |= 0x40;
        if (apu->dmc.irq_pending)          res |= 0x80;

        apu->pending_irq_clear = 1;

        return (open_bus_data & 0x20) | res;
    }

    return open_bus_data;
}

static void apu_reload_length(
    apu_t *apu,
    uint8_t *counter,
    uint8_t channel_mask,
    uint8_t reload_value)
{
    if (apu->pending_length_clock) {
        if (*counter != 0) return;
        apu->pending_length_skip_mask |= channel_mask;
    }

    *counter = reload_value;
}

void apu_cpu_write(
    apu_t *apu,
    uint16_t addr,
    uint8_t value)
{
    switch (addr) {

        case 0x4000:
            apu->p1.duty = (value >> 6) & 3;
            apu->p1.length_halt = (value >> 5) & 1;
            apu->p1.constant_volume = (value >> 4) & 1;
            apu->p1.volume_or_period = value & 15;
            break;

        case 0x4001:
            apu->p1.sweep_enable = value >> 7;
            apu->p1.sweep_period = (value >> 4) & 7;
            apu->p1.sweep_negate = (value >> 3) & 1;
            apu->p1.sweep_shift = value & 7;
            apu->p1.sweep_reload = 1;
            break;

        case 0x4002:
            apu->p1.timer_period =
                (apu->p1.timer_period & 0x700) | value;
            break;

        case 0x4003:
            if (apu->p1.enabled) {
                apu_reload_length(apu, &apu->p1.length_counter,
                                  0x01, length_table[value >> 3]);
            }
            apu->p1.timer_period =
                (apu->p1.timer_period & 0xFF) | ((value & 7) << 8);
            apu->p1.duty_step = 0;
            apu->p1.envelope_start = 1;
            break;

        case 0x4004:
            apu->p2.duty = (value >> 6) & 3;
            apu->p2.length_halt = (value >> 5) & 1;
            apu->p2.constant_volume = (value >> 4) & 1;
            apu->p2.volume_or_period = value & 15;
            break;

        case 0x4005:
            apu->p2.sweep_enable = value >> 7;
            apu->p2.sweep_period = (value >> 4) & 7;
            apu->p2.sweep_negate = (value >> 3) & 1;
            apu->p2.sweep_shift = value & 7;
            apu->p2.sweep_reload = 1;
            break;

        case 0x4006:
            apu->p2.timer_period =
                (apu->p2.timer_period & 0x700) | value;
            break;

        case 0x4007:
            if (apu->p2.enabled) {
                apu_reload_length(apu, &apu->p2.length_counter,
                                  0x02, length_table[value >> 3]);
            }
            apu->p2.timer_period =
                (apu->p2.timer_period & 0xFF) | ((value & 7) << 8);
            apu->p2.duty_step = 0;
            apu->p2.envelope_start = 1;
            break;

        case 0x4008:
            apu->tri.length_halt = value >> 7;
            apu->tri.linear_counter_reload_val = value & 0x7F;
            break;

        case 0x400A:
            apu->tri.timer_period =
                (apu->tri.timer_period & 0x700) | value;
            break;

        case 0x400B:
            if (apu->tri.enabled) {
                apu_reload_length(apu, &apu->tri.length_counter,
                                  0x04, length_table[value >> 3]);
            }
            apu->tri.timer_period =
                (apu->tri.timer_period & 0xFF) | ((value & 7) << 8);
            apu->tri.linear_counter_reload_flag = 1;
            break;

        case 0x400C:
            apu->noise.length_halt = (value >> 5) & 1;
            apu->noise.constant_volume = (value >> 4) & 1;
            apu->noise.volume_or_period = value & 15;
            break;

        case 0x400E:
            apu->noise.mode = value >> 7;
            apu->noise.timer_period =
                noise_timer_periods[value & 15];
            break;

        case 0x400F:
            if (apu->noise.enabled) {
                apu_reload_length(apu, &apu->noise.length_counter,
                                  0x08, length_table[value >> 3]);
            }
            apu->noise.envelope_start = 1;
            break;

        case 0x4010:
            apu->dmc.irq_enable = value >> 7;
            apu->dmc.loop = (value >> 6) & 1;
            apu->dmc.timer_period = dmc_rate_table[value & 15] - 1;

            if (apu->dmc.timer_reloaded_this_cycle) {
                apu->dmc.timer = apu->dmc.timer_period;
            }
            apu->dmc.timer_reloaded_this_cycle = 0;

            if (!apu->dmc.irq_enable) {
                apu->dmc.irq_pending = 0;
                apu_update_irq_line(apu);
            }
            break;

        case 0x4011:
            apu->dmc.value = value & 0x7F;
            break;

        case 0x4012:
            apu->dmc.sample_address = 0xC000 | (value << 6);
            break;

        case 0x4013:
            apu->dmc.sample_length = (value << 4) | 1;
            break;

        case 0x4015: {
            apu->p1.enabled = value & 0x01;
            apu->p2.enabled = value & 0x02;
            apu->tri.enabled = value & 0x04;
            apu->noise.enabled = value & 0x08;

            uint8_t dmc_was_enabled = apu->dmc.enabled;
            apu->dmc.enabled = value & 0x10;

            if (!dmc_was_enabled &&
                apu->dmc.enabled &&
                apu->dmc.buffer_full) {
                apu->dmc.enable_delay =
                    (apu->total_cycles & 1) ? 2 : 3;
                apu->dmc.enable_reload_gate = 0;
            }

            if (!apu->p1.enabled)    apu->p1.length_counter = 0;
            if (!apu->p2.enabled)    apu->p2.length_counter = 0;
            if (!apu->tri.enabled)   apu->tri.length_counter = 0;
            if (!apu->noise.enabled) apu->noise.length_counter = 0;

            if (!apu->dmc.enabled) {
                apu->dmc.enable_delay = 0;
                apu->dmc.enable_reload_gate = 0;

                if (apu->dmc.disable_delay == 0) {
                    if ((apu->total_cycles & 1) == 0) {
                        apu->dmc.disable_delay = 3;
                    } else {
                        apu->dmc.disable_delay = 2;
                    }
                }
            } else if (apu->dmc.current_length == 0) {
                apu->dmc.implicit_abort_active = 0;
                apu->dmc.current_address = apu->dmc.sample_address;
                apu->dmc.current_length = apu->dmc.sample_length;

                if (!apu->dmc.buffer_full &&
                    apu->dmc.current_length > 0) {
                    dmc_schedule_load(apu);
                }
            }

            apu->pending_length_clock = 0;
            apu->dmc.irq_pending = 0;
            apu_update_irq_line(apu);
            break;
        }

        case 0x4017:
            apu->reg_4017_frame_counter = value;
            apu->frame_counter_mode = (value >> 7) & 1;

            if (value & 0x40) {
                apu->frame_counter_irq = 0;
                apu_update_irq_line(apu);
            }

            apu->frame_counter_reset_pending = 1;
            apu->reset_delay =
                (apu->total_cycles & 1) ? 3 : 4;
            break;
    }
}

// ------------------------------------------------------------
// SAMPLE DRAIN
// ------------------------------------------------------------

uint32_t apu_drain_samples(
    apu_t *apu,
    int16_t *out,
    uint32_t max_samples)
{
    uint32_t n = (apu->sample_count < max_samples)
        ? apu->sample_count
        : max_samples;

    if (n > 0 && out) {
        memcpy(out, apu->sample_buffer, n * sizeof(int16_t));

        if (n < apu->sample_count) {
            memmove(apu->sample_buffer,
                    &apu->sample_buffer[n],
                    (apu->sample_count - n) * sizeof(int16_t));
            apu->sample_count -= n;
        } else {
            apu->sample_count = 0;
        }
    }

    return n;
}