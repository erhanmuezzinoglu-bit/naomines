#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_DMC_DMA_NONE   0
#define APU_DMC_DMA_LOAD   1
#define APU_DMC_DMA_RELOAD 2

#define DMC_DMA_RUNTIME_ACTIVE                 0x80u
#define DMC_DMA_RUNTIME_ABORT                  0x81u
#define DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE  0xA0u
#define DMC_DMA_RUNTIME_IMPLICIT_ACTIVE        0x82u
#define DMC_DMA_RUNTIME_IMPLICIT_ABORT         0x83u

typedef struct {
    uint8_t duty;
    uint8_t length_halt;
    uint8_t constant_volume;
    uint8_t volume_or_period;

    uint8_t sweep_enable;
    uint8_t sweep_period;
    uint8_t sweep_negate;
    uint8_t sweep_shift;
    uint8_t sweep_reload;
    uint8_t sweep_divider;

    uint16_t timer_period;
    uint16_t timer;
    uint8_t duty_step;
    uint8_t length_counter;

    uint8_t envelope_start;
    uint8_t envelope_divider;
    uint8_t envelope_decay;

    uint8_t enabled;
} pulse_t;

typedef struct {
    uint8_t enabled;
    uint16_t timer_period;
    uint16_t timer;
    uint8_t duty_step;

    uint8_t length_counter;
    uint8_t length_halt;

    uint8_t linear_counter_reload_val;
    uint8_t linear_counter;
    uint8_t linear_counter_reload_flag;
} triangle_t;

typedef struct {
    uint8_t enabled;
    uint8_t length_halt;
    uint8_t constant_volume;
    uint8_t volume_or_period;

    uint16_t timer_period;
    uint16_t timer;

    uint8_t length_counter;

    uint8_t envelope_start;
    uint8_t envelope_divider;
    uint8_t envelope_decay;

    uint8_t mode;
    uint16_t shift_register;
} noise_t;

typedef struct {
    uint8_t enabled;
    uint8_t value;

    uint16_t sample_address;
    uint16_t sample_length;
    uint16_t current_address;
    uint16_t current_length;

    uint8_t shift_register;
    uint8_t bit_count;
    uint16_t timer;
    uint16_t timer_period;

    uint8_t irq_enable;
    uint8_t irq_pending;
    uint8_t loop;

    uint8_t dma_request;
    uint8_t dma_kind;
    uint8_t dma_delay;
    uint8_t dma_delay_parity;

    uint8_t sample_buffer;
    bool buffer_full;

    uint8_t disable_delay;
    uint8_t implicit_stop_delay;
    uint8_t enable_delay;
    uint8_t enable_reload_gate;

    uint8_t implicit_abort_pending;
    uint8_t implicit_abort_active;
    uint8_t loop_implicit_delay;

    uint8_t timer_reloaded_this_cycle;
} dmc_t;

typedef struct {
    uint8_t reg_4017_frame_counter;
    uint8_t frame_counter_irq;
    uint8_t frame_counter_mode;

    pulse_t p1;
    pulse_t p2;
    triangle_t tri;
    noise_t noise;
    dmc_t dmc;

    uint32_t frame_seq_count;
    int reset_delay;
    uint64_t total_cycles;

    uint8_t pending_irq_clear;

    uint8_t pending_length_clock;
    uint8_t pending_length_halt_mask;
    uint8_t pending_length_skip_mask;

    uint8_t pending_frame_irq_reassert;
    uint8_t frame_counter_reset_pending;

    int16_t sample_buffer[8192];
    uint32_t sample_count;
    uint32_t sample_accumulator;
} apu_t;

void apu_init(apu_t *apu);
void apu_reset(apu_t *apu);

uint8_t apu_cpu_read(
    apu_t *apu,
    uint16_t addr,
    uint8_t open_bus_data
);

void apu_cpu_write(
    apu_t *apu,
    uint16_t addr,
    uint8_t value
);

void apu_step(
    apu_t *apu,
    int cpu_cycles
);

int apu_irq_line(const apu_t *apu);

/* Siradaki CPU-gorunur APU olayina kalan cycle sayisi (>=1). Tembel senkron icin. */
int apu_cycles_to_next_event(const apu_t *apu);

void apu_dmc_arm_implicit_abort(apu_t *apu);

int apu_dmc_enable_blocks_halt(apu_t *apu);

uint32_t apu_drain_samples(
    apu_t *apu,
    int16_t *out,
    uint32_t max_samples
);

#ifdef __cplusplus
}
#endif

#endif