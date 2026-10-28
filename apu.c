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

static void apu_update_irq_line(apu_t *apu)
{
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
    *timer = (uint16_t)(period - (m % p1));
    return 1u + m / p1;
}

static void apu_bulk_advance(apu_t *apu, uint32_t s)
{
    const uint64_t t0 = apu->total_cycles;
    const uint32_t evens = (uint32_t)(((t0 + s) >> 1) - (t0 >> 1));

    apu->dmc.timer_reloaded_this_cycle = 0;

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
        const uint8_t shift_amt = n->mode ? 6 : 1;
        while (r--) {
            uint16_t feedback = (n->shift_register & 1) ^ ((n->shift_register >> shift_amt) & 1);
            n->shift_register >>= 1;
            n->shift_register |= (uint16_t)(feedback << 14);
        }
    }

    /* DMC sayaci: bu aralikta reload yok (s <= timer) */
    apu->dmc.timer = (uint16_t)(apu->dmc.timer - s);

    apu->frame_seq_count += s;
    apu->total_cycles = t0 + s;
    apu->sample_accumulator += APU_OUTPUT_HZ * s;
}

/* Siradaki "CPU'ya gorunebilen" APU olayina kalan cycle sayisi (>=1): sessiz degilse 1;
 * DMC sayac reload'i (DMA istegi/IRQ olusturabilir) ve frame-sequencer olaylari (IRQ). */
int apu_cycles_to_next_event(const apu_t *apu)
{
    if (!apu_quiescent(apu)) return 1;

    uint32_t e = (uint32_t)apu->dmc.timer + 1u;
    uint32_t f = apu_frame_event_distance(apu);
    if (f < e) e = f;
    return (int)e;
}

void apu_step(apu_t *apu, int cpu_cycles)
{
#if EXP_ABLATE
    /* M1/M4 (SADECE OLCUM): APU adimi kapali. */
    if (EXP_APU_OFF) return;
#endif
    int remaining = cpu_cycles;

    while (remaining > 0) {
        if (remaining > 1 && apu_quiescent(apu)) {
            /* ilk olay cycle'inin indeksi (1 tabanli): DMC reload, frame olayi, ornek uretimi */
            uint32_t j = (uint32_t)apu->dmc.timer + 1u;
            uint32_t f = apu_frame_event_distance(apu);
            if (f < j) j = f;
            uint32_t smp = (APU_CPU_CLOCK_HZ - apu->sample_accumulator + APU_OUTPUT_HZ - 1u) / APU_OUTPUT_HZ;
            if (smp < j) j = smp;

            uint32_t skip = j - 1u;                    /* olay icermeyen bastaki cycle sayisi */
            if (skip > (uint32_t)remaining) skip = (uint32_t)remaining;
            if (skip > 0u) {
                apu_bulk_advance(apu, skip);
                remaining -= (int)skip;
            }
            if (remaining > 0) {
                apu_cycle(apu);
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