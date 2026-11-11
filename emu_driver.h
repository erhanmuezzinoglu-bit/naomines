#ifndef EMU_DRIVER_H
#define EMU_DRIVER_H

#include <stdint.h>

#include "fake6502.h"
#include "ppu.h"
#include "nes_input.h"
#include "nes_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

void emu_driver_step_ppu_sampled(ppu_t *ppu, uint32_t cycles);

/* PPU'yu mevcut CPU cycle'ina kadar senkronla (tembel modda bekleyen dot'lari calistirir). */
void emu_driver_sync_ppu(void);

/* APU'yu mevcut CPU cycle'ina kadar senkronla (tembel modda bekleyen cycle'lari calistirir). */
void emu_driver_sync_apu(void);

/* DMA'da calinan cycle sonrasi: deadline'i gelen tembel PPU/APU olaylarini isle. */
void emu_driver_dma_cycle_events(void);

/* Bos dongu atlamasinin mutlak clockticks siniri (kare butcesi). 0 = atlama kapali. main.c her karede
 * emu_driver_ff_set_stop(cpu.emu.clockticks + steps_per_frame) cagirmalidir. */
void emu_driver_ff_set_stop(int abs_ticks);
extern unsigned long g_ff_cycles_total;   /* atlanan toplam cycle (istatistik) */
extern unsigned long g_ff_events_total;   /* atlama sayisi */


void emu_driver_init(fake6502_context *cpu,
                     ppu_t *ppu,
                     nes_input_t *in,
                     nes_bus_t *bus);

void emu_driver_reset_from_vector(void);

uint32_t emu_driver_step_1(void);

void emu_driver_request_irq(void);
void emu_driver_clear_irq(void);

#ifdef __cplusplus
}
#endif

#endif