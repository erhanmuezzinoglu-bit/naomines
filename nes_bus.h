#ifndef NES_BUS_H
#define NES_BUS_H

#include <stdint.h>
#include "fake6502.h"
#include "ppu.h"
#include "trace.h"
#include "nes_input.h"
#include "apu.h" 

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes_bus
{
    // Bellek Alanları
    uint8_t mem[0x10000];
    uint8_t cart_sram[0x2000];

    // Bileşen Bağlantıları
    ppu_t *ppu;
    nes_input_t *in;
    trace_state_t *t; 

    // --- OPEN BUS & DYNAMIC DATA ---
    uint8_t open_bus_value;

    // CPU'nun en son okuduğu adres (DMC DMA halt/alignment dummy read için)
    uint16_t last_cpu_read_addr;

    // --- FAST PAGE TABLOSU ---
    // 64KB / 2KB = 32 sayfa.
    uint8_t *read_table[32];

    // --- APU Durumu ve Senkronizasyon ---
    apu_t apu;
    
    // Catch-up senkronizasyonu için CPU çevrim takibi
    uint32_t last_apu_sync_tick; 
    uint32_t last_ppu_sync_tick; // PPU hassas zamanlaması için eklendi

    /* APU Register Durumları */
    uint8_t apu_4017_frame_counter;

    /* Debug counters */
    uint32_t dbg_oam_dma_count;

    /* TEMP DEBUG: controller IO statistics ($4016) */
    uint32_t dbg_4016_reads;
    uint32_t dbg_4016_writes;
    uint8_t  dbg_4016_last_write;

    /* Watch / HUD İzleme */
    uint8_t  watch_ppu_2000;
    uint8_t  watch_ppu_2001;
    uint8_t  watch_ppu_2005_a, watch_ppu_2005_b;
    uint8_t  watch_ppu_2006_hi, watch_ppu_2006_lo;
    uint8_t  watch_ppu_2007;
    uint8_t  watch_ppu_2002_last;
    
    // PPU'nun içindeki 'w' latch durumu
    uint8_t  watch_ppu_w; 

    /* Last written $4014 value (DMA page) */
    uint8_t  watch_oam_dma_page;

    // Trace ve loglama
    trace_state_t *trace; 
} nes_bus_t;

/* --- Temel Fonksiyonlar --- */
void nes_bus_init(nes_bus_t *b, ppu_t *ppu, nes_input_t *in, trace_state_t *t);
uint8_t nes_bus_read(nes_bus_t *b, fake6502_context *ctx, uint16_t addr);
void nes_bus_write(nes_bus_t *b, fake6502_context *ctx, uint16_t addr, uint8_t value);
void nes_bus_dma_dmc_step(nes_bus_t *bus, fake6502_context *ctx);

/* --- ROM/Mapper Fonksiyonları (nes_rom.c içinde tanımlı) --- */
void nes_rom_write(uint16_t addr, uint8_t value, uint8_t mem_64k[0x10000]);

#ifdef __cplusplus
}
#endif

#endif