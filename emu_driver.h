#ifndef EMU_DRIVER_H
#define EMU_DRIVER_H

#include <stdint.h>

#include "exp_config.h"
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

/* Tani sayaclari (bkz. emu_driver.c). */
extern unsigned long g_diag_status_reads, g_diag_io;
/* TANI (EXP_DIAG_HUD; davranisi degistirmez): 0 PPU senkronu, 1 APU senkronu, 2 PPU yazmac yazimi, 3 APU/G-C ($4000-$401F) yazimi,
 * 4 mapper yazimi ($4020+, PRG-RAM haric), 5 APU/G-C okumasi, 6 PRG 8K kopyasi, 7 CHR 1K kopyasi. 6-7 mapper.c'de artar. */
extern unsigned long g_diag_k[10];   /* 8: hizli yolda yapilan $2002 okumasi */
extern unsigned long g_ffp_cycles_total;   /* $2002 yoklama atlamasiyla gecilen cycle */
/* Olcum kipi (EXP_DIAG_PROF) ve CPU sayaclari: yalniz ilgili derlemede tanimlidir (bkz. emu_driver.c). */
void emu_driver_prof_set_mode(int m);
extern unsigned long g_st_rf_calls, g_st_rf_instr, g_st_slow;
#if defined(EXP_DIAG_PROF) && EXP_DIAG_PROF
void emu_driver_apu_step(apu_t *a, int n);
#define APU_STEP(a, n) emu_driver_apu_step((a), (n))
#else
#define APU_STEP(a, n) apu_step((a), (n))
#endif

/* Yoklama atlamasi tanisi: yalniz EXP_DIAG_HUD=1 derlemesinde tanimlidir (bkz. emu_driver.c). */
extern unsigned long g_pdiag[8];
extern uint16_t g_pdiag_rpc, g_pdiag_bpc;

#ifdef __cplusplus
}
#endif

#endif