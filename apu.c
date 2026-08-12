#include "apu.h"
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
    {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1}
};

static const uint8_t triangle_sequence[32] = {
    15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
};

static const uint16_t noise_timer_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

// NOT: Bu değerler CPU cycle cinsindendir. DMC timer artık CPU rate'te
// (her CPU cycle) clock'landığı için period = tablo değeri - 1 kullanılır.
static const uint16_t dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54
};

static void apu_update_irq_line(apu_t *apu) {
    if (apu->frame_counter_irq || apu->dmc.irq_pending) {
        emu_driver_request_irq();
    } else {
        emu_driver_clear_irq();
    }
}

// ============================================================
// PULSE
// ============================================================

static uint16_t pulse_target_period(const pulse_t *p, int is_pulse1) {
    uint16_t shift = p->timer_period >> p->sweep_shift;
    if (p->sweep_negate)
        return is_pulse1 ? p->timer_period - shift - 1 : p->timer_period - shift;
    return p->timer_period + shift;
}

static int pulse_is_muted(const pulse_t *p, int is_pulse1) {
    if (p->timer_period < 8) return 1;
    if (pulse_target_period(p, is_pulse1) > 0x7FF) return 1;
    return 0;
}

static void pulse_clock_timer(pulse_t *p) {
    if (p->timer == 0) {
        p->timer = p->timer_period;
        p->duty_step = (p->duty_step + 1) & 7;
    } else {
        p->timer--;
    }
}

static void pulse_clock_envelope(pulse_t *p) {
    if (p->envelope_start) {
        p->envelope_start    = 0;
        p->envelope_decay    = 15;
        p->envelope_divider  = p->volume_or_period;
    } else {
        if (p->envelope_divider == 0) {
            p->envelope_divider = p->volume_or_period;
            if (p->envelope_decay > 0)   p->envelope_decay--;
            else if (p->length_halt)     p->envelope_decay = 15;
        } else {
            p->envelope_divider--;
        }
    }
}

static void pulse_clock_length(pulse_t *p) {
    if (!p->length_halt && p->length_counter > 0) p->length_counter--;
}

// FIX #2: Sweep mute kosulu duzeltildi.
static void pulse_clock_sweep(pulse_t *p, int is_pulse1) {
    uint16_t target = pulse_target_period(p, is_pulse1);
    if (p->sweep_divider == 0 && p->sweep_enable
            && p->sweep_shift > 0 && target <= 0x7FF) {
        p->timer_period = target;
    }
    if (p->sweep_divider == 0 || p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload  = 0;
    } else {
        p->sweep_divider--;
    }
}

static uint8_t pulse_output(const pulse_t *p, int is_pulse1) {
    if (!p->enabled || p->length_counter == 0 || pulse_is_muted(p, is_pulse1)) return 0;
    if (duty_table[p->duty][p->duty_step] == 0) return 0;
    return p->constant_volume ? p->volume_or_period : p->envelope_decay;
}

// ============================================================
// TRIANGLE
// ============================================================

static void triangle_clock_timer(triangle_t *t) {
    if (t->timer == 0) {
        t->timer = t->timer_period;
        if (t->length_counter > 0 && t->linear_counter > 0 && t->timer_period > 2)
            t->duty_step = (t->duty_step + 1) & 0x1F;
    } else {
        t->timer--;
    }
}

static void triangle_clock_linear(triangle_t *t) {
    if (t->linear_counter_reload_flag) t->linear_counter = t->linear_counter_reload_val;
    else if (t->linear_counter > 0)    t->linear_counter--;
    if (!t->length_halt) t->linear_counter_reload_flag = 0;
}

static void triangle_clock_length(triangle_t *t) {
    if (!t->length_halt && t->length_counter > 0) t->length_counter--;
}

static uint8_t triangle_output(const triangle_t *t) {
    if (!t->enabled || t->length_counter == 0 || t->linear_counter == 0) return 0;
    return triangle_sequence[t->duty_step];
}

// ============================================================
// NOISE
// ============================================================

static void noise_clock_timer(noise_t *n) {
    if (n->timer == 0) {
        n->timer = n->timer_period;
        uint8_t  shift_amt = n->mode ? 6 : 1;
        uint16_t feedback  = (n->shift_register & 1) ^ ((n->shift_register >> shift_amt) & 1);
        n->shift_register >>= 1;
        n->shift_register  |= (feedback << 14);
    } else {
        n->timer--;
    }
}

static void noise_clock_envelope(noise_t *n) {
    if (n->envelope_start) {
        n->envelope_start    = 0;
        n->envelope_decay    = 15;
        n->envelope_divider  = n->volume_or_period;
    } else {
        if (n->envelope_divider == 0) {
            n->envelope_divider = n->volume_or_period;
            if (n->envelope_decay > 0)  n->envelope_decay--;
            else if (n->length_halt)    n->envelope_decay = 15;
        } else {
            n->envelope_divider--;
        }
    }
}

static void noise_clock_length(noise_t *n) {
    if (!n->length_halt && n->length_counter > 0) n->length_counter--;
}

static uint8_t noise_output(const noise_t *n) {
    if (!n->enabled || n->length_counter == 0) return 0;
    if (n->shift_register & 1) return 0;
    return n->constant_volume ? n->volume_or_period : n->envelope_decay;
}

// ============================================================
// DMC
// ============================================================

static void dmc_clock_timer(apu_t *apu) {
    dmc_t *d = &apu->dmc;
    if (d->timer == 0) {
        d->timer = d->timer_period;

        if (d->bit_count > 0) {
            if (d->shift_register & 1) {
                if (d->value <= 125) d->value += 2;
            } else {
                if (d->value >= 2)   d->value -= 2;
            }
            d->shift_register >>= 1;
            d->bit_count--;
        }

        if (d->bit_count == 0) {
            d->bit_count = 8;
            if (d->buffer_full) {
                d->shift_register = d->sample_buffer;
                d->buffer_full    = false;
                if (d->current_length > 0) {
                    d->dma_request = 1;
                }
            } else {
                if (d->current_length == 0) {
                    if (d->loop) {
                        d->current_address = d->sample_address;
                        d->current_length  = d->sample_length;
                        d->dma_request     = 1;
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

// ============================================================
// FRAME SEQUENCER HELPERS
// ============================================================

static void apu_quarter_frame(apu_t *apu) {
    pulse_clock_envelope(&apu->p1);
    pulse_clock_envelope(&apu->p2);
    noise_clock_envelope(&apu->noise);
    triangle_clock_linear(&apu->tri);
}

// FIX (blargg 05/06 len_timing): apu_half_frame'de length counter clock'u
// ANINDA yapilmaz; bunun yerine pending_length_clock bayragi set edilir ve
// length bir SONRAKI cycle'da (apu_step basinda) decrement edilir. Boylece
// ayni cycle'daki $4015 okumasi length'i ESKI degeriyle gorur (blargg 05
// "should be playing"). Envelope, linear ve sweep HEMEN clock'lanir (bunlar
// $4015 length okumasini etkilemez).
static void apu_half_frame(apu_t *apu) {
    apu_quarter_frame(apu);
    pulse_clock_sweep(&apu->p1, 1);
    pulse_clock_sweep(&apu->p2, 0);
    apu->pending_length_clock = 1;
}

// ============================================================
// MIXER
// ============================================================

static int16_t apu_mix(uint8_t p1, uint8_t p2, uint8_t tri, uint8_t noise, uint8_t dmc) {
    float pulse_out = 0.0f;
    if ((p1 + p2) > 0)
        pulse_out = 95.88f / (8128.0f / (float)(p1 + p2) + 100.0f);

    float tnd_out   = 0.0f;
    float tnd_denom = (float)tri / 8227.0f + (float)noise / 12241.0f + (float)dmc / 22638.0f;
    if (tnd_denom > 0.0f)
        tnd_out = 159.79f / (1.0f / tnd_denom + 100.0f);

    return (int16_t)((pulse_out + tnd_out) * 30000.0f);
}

// ============================================================
// INIT / RESET
// ============================================================

void apu_init(apu_t *apu) {
    memset(apu, 0, sizeof(*apu));
    apu->noise.shift_register   = 1;
    apu->total_cycles           = 0;
    apu->reg_4017_frame_counter = 0x00;
    apu->frame_counter_mode     = 0;
    apu->frame_seq_count        = 0;
    apu->pending_length_clock   = 0;
}

void apu_reset(apu_t *apu) {
    apu->p1.enabled    = 0;
    apu->p2.enabled    = 0;
    apu->tri.enabled   = 0;
    apu->noise.enabled = 0;
    apu->dmc.enabled   = 0;

    apu->p1.length_counter    = 0;
    apu->p2.length_counter    = 0;
    apu->tri.length_counter   = 0;
    apu->noise.length_counter = 0;
    apu->dmc.current_length   = 0;

    apu->frame_counter_irq = 0;
    apu->pending_length_clock = 0;
    apu_update_irq_line(apu);
}

// ============================================================
// STEP
// ============================================================

void apu_step(apu_t *apu, int cpu_cycles) {
    for (int i = 0; i < cpu_cycles; i++) {
        apu->total_cycles++;

        // FIX (blargg 05/06): Bir onceki cycle'da set edilen ertelenmis length
        // clock'u SIMDI (okumadan sonraki cycle'da) uygula.
        if (apu->pending_length_clock) {
            apu->pending_length_clock = 0;
            pulse_clock_length(&apu->p1);
            pulse_clock_length(&apu->p2);
            noise_clock_length(&apu->noise);
            triangle_clock_length(&apu->tri);
        }

        // $4017 yazma gecikmesi (NTSC: tek cycle → 4, cift cycle → 3)
        if (apu->reset_delay > 0) {
            if (--apu->reset_delay == 0) {
                apu->frame_seq_count = 0;
                if (apu->frame_counter_mode == 1) {
                    apu_half_frame(apu);
                }
            }
        }

        // Pulse timer'lari APU clock hizinda (CPU/2) calisir
        if ((apu->total_cycles & 1) == 0) {
            pulse_clock_timer(&apu->p1);
            pulse_clock_timer(&apu->p2);
        }

        // FIX: DMC timer'i GERCEK donanim gibi HER CPU cycle clock'lanir.
        dmc_clock_timer(apu);

        // Triangle ve noise CPU hizinda calisir
        triangle_clock_timer(&apu->tri);
        noise_clock_timer(&apu->noise);

        // --- Frame Counter Sequencer ---
        apu->frame_seq_count++;

        if (apu->frame_counter_mode == 0) {
            // 4-Step Mode (NTSC)
            if (apu->frame_seq_count == 7457) {
                apu_quarter_frame(apu);
            }
            else if (apu->frame_seq_count == 14913) {
                apu_half_frame(apu);
            }
            else if (apu->frame_seq_count == 22371) {
                apu_quarter_frame(apu);
            }
            else if (apu->frame_seq_count == 29829) {
                // DENEME (blargg 05 "second length too late"): half_frame'i
                // IRQ ile AYNI cycle'a (29829, gercek NTSC step 4) tasi.
                // Boylece second length pending → 29830'da duser → okuma
                // 29831 "playing", 29832 "silent" olur.
                apu_half_frame(apu);
                // 1. IRQ darbesi
                if (!(apu->reg_4017_frame_counter & 0x40)) {
                    apu->frame_counter_irq = 1;
                    apu_update_irq_line(apu);
                }
            }
            else if (apu->frame_seq_count == 29830) {
                // 2. IRQ darbesi + frame reset (half_frame ARTIK burada DEGIL)
                if (!(apu->reg_4017_frame_counter & 0x40)) {
                    apu->frame_counter_irq = 1;
                    apu_update_irq_line(apu);
                }
                apu->frame_seq_count = 0;
            }
        } else {
            // 5-Step Mode (NTSC) — DOKUNULMADI
            if      (apu->frame_seq_count == 7457)  apu_quarter_frame(apu);
            else if (apu->frame_seq_count == 14913) apu_half_frame(apu);
            else if (apu->frame_seq_count == 22371) apu_quarter_frame(apu);
            else if (apu->frame_seq_count == 37281) apu_half_frame(apu);
            else if (apu->frame_seq_count == 37282) {
                apu->frame_seq_count = 0;
            }
        }

        // Ses ornekleme (CPU clock → output sample rate donusumu)
        apu->sample_accumulator += APU_OUTPUT_HZ;
        if (apu->sample_accumulator >= APU_CPU_CLOCK_HZ) {
            apu->sample_accumulator -= APU_CPU_CLOCK_HZ;
            if (apu->sample_count < 4096) {
                apu->sample_buffer[apu->sample_count++] = apu_mix(
                    pulse_output(&apu->p1, 1),
                    pulse_output(&apu->p2, 0),
                    triangle_output(&apu->tri),
                    noise_output(&apu->noise),
                    apu->dmc.value);
            }
        }
    }
}

// ============================================================
// CPU READ / WRITE
// ============================================================

uint8_t apu_cpu_read(apu_t *apu, uint16_t addr, uint8_t open_bus_data) {
    if (addr == 0x4015) {
        uint8_t res = 0;
        if (apu->p1.length_counter > 0)    res |= 0x01;
        if (apu->p2.length_counter > 0)    res |= 0x02;
        if (apu->tri.length_counter > 0)   res |= 0x04;
        if (apu->noise.length_counter > 0) res |= 0x08;
        if (apu->dmc.current_length > 0)   res |= 0x10;
        if (apu->frame_counter_irq)        res |= 0x40;
        if (apu->dmc.irq_pending)          res |= 0x80;

        apu->frame_counter_irq = 0;
        apu_update_irq_line(apu);
        return (open_bus_data & 0x20) | res;
    }
    return open_bus_data;
}

void apu_cpu_write(apu_t *apu, uint16_t addr, uint8_t value) {
    switch (addr) {

        // --- Pulse 1 ---
        case 0x4000:
            apu->p1.duty             = (value >> 6) & 3;
            apu->p1.length_halt      = (value >> 5) & 1;
            apu->p1.constant_volume  = (value >> 4) & 1;
            apu->p1.volume_or_period = value & 15;
            break;
        case 0x4001:
            apu->p1.sweep_enable = value >> 7;
            apu->p1.sweep_period = (value >> 4) & 7;
            apu->p1.sweep_negate = (value >> 3) & 1;
            apu->p1.sweep_shift  = value & 7;
            apu->p1.sweep_reload = 1;
            break;
        case 0x4002:
            apu->p1.timer_period = (apu->p1.timer_period & 0x700) | value;
            break;
        case 0x4003:
            if (apu->p1.enabled)
                apu->p1.length_counter = length_table[value >> 3];
            apu->p1.timer_period   = (apu->p1.timer_period & 0xFF) | ((value & 7) << 8);
            apu->p1.duty_step      = 0;
            apu->p1.envelope_start = 1;
            break;

        // --- Pulse 2 ---
        case 0x4004:
            apu->p2.duty             = (value >> 6) & 3;
            apu->p2.length_halt      = (value >> 5) & 1;
            apu->p2.constant_volume  = (value >> 4) & 1;
            apu->p2.volume_or_period = value & 15;
            break;
        case 0x4005:
            apu->p2.sweep_enable = value >> 7;
            apu->p2.sweep_period = (value >> 4) & 7;
            apu->p2.sweep_negate = (value >> 3) & 1;
            apu->p2.sweep_shift  = value & 7;
            apu->p2.sweep_reload = 1;
            break;
        case 0x4006:
            apu->p2.timer_period = (apu->p2.timer_period & 0x700) | value;
            break;
        case 0x4007:
            if (apu->p2.enabled)
                apu->p2.length_counter = length_table[value >> 3];
            apu->p2.timer_period   = (apu->p2.timer_period & 0xFF) | ((value & 7) << 8);
            apu->p2.duty_step      = 0;
            apu->p2.envelope_start = 1;
            break;

        // --- Triangle ---
        case 0x4008:
            apu->tri.length_halt              = (value >> 7);
            apu->tri.linear_counter_reload_val = value & 0x7F;
            break;
        case 0x400A:
            apu->tri.timer_period = (apu->tri.timer_period & 0x700) | value;
            break;
        case 0x400B:
            if (apu->tri.enabled)
                apu->tri.length_counter = length_table[value >> 3];
            apu->tri.timer_period             = (apu->tri.timer_period & 0xFF) | ((value & 7) << 8);
            apu->tri.linear_counter_reload_flag = 1;
            break;

        // --- Noise ---
        case 0x400C:
            apu->noise.length_halt      = (value >> 5) & 1;
            apu->noise.constant_volume  = (value >> 4) & 1;
            apu->noise.volume_or_period = value & 15;
            break;
        case 0x400E:
            apu->noise.mode         = value >> 7;
            apu->noise.timer_period = noise_timer_periods[value & 15];
            break;
        case 0x400F:
            if (apu->noise.enabled)
                apu->noise.length_counter = length_table[value >> 3];
            apu->noise.envelope_start = 1;
            break;

        // --- DMC ---
        case 0x4010:
            apu->dmc.irq_enable = value >> 7;
            apu->dmc.loop       = (value >> 6) & 1;
            apu->dmc.timer_period = dmc_rate_table[value & 15] - 1;
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

        // --- Status ($4015) ---
        case 0x4015:
            apu->p1.enabled    = value & 0x01;
            apu->p2.enabled    = value & 0x02;
            apu->tri.enabled   = value & 0x04;
            apu->noise.enabled = value & 0x08;
            apu->dmc.enabled   = value & 0x10;

            if (!apu->p1.enabled)    apu->p1.length_counter    = 0;
            if (!apu->p2.enabled)    apu->p2.length_counter    = 0;
            if (!apu->tri.enabled)   apu->tri.length_counter   = 0;
            if (!apu->noise.enabled) apu->noise.length_counter = 0;

            if (!apu->dmc.enabled) {
                apu->dmc.current_length = 0;
                apu->dmc.dma_request    = 0;
                apu->dmc.buffer_full    = false;
            } else if (apu->dmc.current_length == 0) {
                apu->dmc.current_address = apu->dmc.sample_address;
                apu->dmc.current_length  = apu->dmc.sample_length;
                apu->dmc.dma_request     = 1;
            }

            apu->dmc.irq_pending = 0;
            apu_update_irq_line(apu);
            break;

        // --- Frame Counter ($4017) ---
        case 0x4017:
            apu->reg_4017_frame_counter = value;
            apu->frame_counter_mode     = (value >> 7) & 1;
            if (value & 0x40) {
                apu->frame_counter_irq = 0;
                apu_update_irq_line(apu);
            }
            apu->reset_delay = (apu->total_cycles & 1) ? 4 : 3;
            break;
    }
}

// ============================================================
// SAMPLE DRAIN
// ============================================================

uint32_t apu_drain_samples(apu_t *apu, int16_t *out, uint32_t max_samples) {
    uint32_t n = (apu->sample_count < max_samples) ? apu->sample_count : max_samples;
    if (n > 0 && out) {
        memcpy(out, apu->sample_buffer, n * sizeof(int16_t));
        if (n < apu->sample_count) {
            memmove(apu->sample_buffer, &apu->sample_buffer[n],
                    (apu->sample_count - n) * sizeof(int16_t));
            apu->sample_count -= n;
        } else {
            apu->sample_count = 0;
        }
    }
    return n;
}