#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  duty;
    uint8_t  length_halt;
    uint8_t  constant_volume;
    uint8_t  volume_or_period;

    uint8_t  sweep_enable;
    uint8_t  sweep_period;
    uint8_t  sweep_negate;
    uint8_t  sweep_shift;
    uint8_t  sweep_reload;
    uint8_t  sweep_divider;

    uint16_t timer_period;
    uint16_t timer;
    uint8_t  duty_step;
    uint8_t  length_counter;

    uint8_t  envelope_start;
    uint8_t  envelope_divider;
    uint8_t  envelope_decay;

    uint8_t  enabled;
} pulse_t;

typedef struct {
    uint8_t  enabled;
    uint16_t timer_period;
    uint16_t timer;
    uint8_t  duty_step;
    
    uint8_t  length_counter;
    uint8_t  length_halt;

    uint8_t  linear_counter_reload_val;
    uint8_t  linear_counter;
    uint8_t  linear_counter_reload_flag;
} triangle_t;

typedef struct {
    uint8_t  enabled;
    uint8_t  length_halt;
    uint8_t  constant_volume;
    uint8_t  volume_or_period;

    uint16_t timer_period;
    uint16_t timer;

    uint8_t  length_counter;

    uint8_t  envelope_start;
    uint8_t  envelope_divider;
    uint8_t  envelope_decay;

    uint8_t  mode;
    uint16_t shift_register; // 15-bit LFSR
} noise_t;

typedef struct {
    uint8_t  enabled;
    uint8_t  value;           // Mevcut output değeri (0-127)
    uint16_t sample_address;  // $4012
    uint16_t sample_length;   // $4013
    uint16_t current_address;
    uint16_t current_length;
    
    uint8_t  shift_register;
    uint8_t  bit_count;
    uint16_t timer;           // Rate tablosuna göre azalan zamanlayıcı
    uint16_t timer_period;    // Seçilen hız (Rate)
    
    uint8_t  irq_enable;      // $4010 bit 7
    uint8_t  irq_pending;     // Aktif IRQ durumu
    uint8_t  loop;            // $4010 bit 6
    
    uint8_t  dma_request;     // Bus'tan veri isteniyor mu?
    uint8_t  sample_buffer;   // Okunan ama henüz shift register'a girmemiş bayt
    bool     buffer_full;     // Buffer dolu mu?
} dmc_t;

typedef struct {
    uint8_t reg_4015_enable;
    uint8_t reg_4017_frame_counter;
    uint8_t frame_counter_irq;
    uint8_t frame_counter_mode;

    pulse_t p1;
    pulse_t p2;
    triangle_t tri;
    noise_t noise;
    dmc_t dmc;

    // Senkronizasyon ve Sayaçlar
    uint32_t frame_seq_count;
    uint8_t  frame_counter_index;
    int      reset_delay;
    uint64_t total_cycles;

    uint8_t  pending_irq_clear;
    uint8_t  pending_length_clock;   // YENİ: length clock 1 cycle ertelenir
                                     // (blargg 05/06 len_timing: $4015 okumasi
                                     //  length clock'tan ONCE gerceklesmeli)

    // Ses Çıkış Buffer'ı
    int16_t  sample_buffer[4096];
    uint32_t sample_count;
    uint32_t sample_accumulator;

} apu_t;

void apu_init(apu_t *apu);
void apu_reset(apu_t *apu);

// AccuracyCoin Fix: Open Bus desteği için open_bus_data parametresi eklendi
uint8_t apu_cpu_read(apu_t *apu, uint16_t addr, uint8_t open_bus_data);
void apu_cpu_write(apu_t *apu, uint16_t addr, uint8_t value);

void apu_step(apu_t *apu, int cpu_cycles);
void apu_clock_length_counters(apu_t *apu);
void apu_set_irq_flag(apu_t *apu);

uint32_t apu_drain_samples(apu_t *apu, int16_t *out, uint32_t max_samples);

#ifdef __cplusplus
}
#endif

#endif