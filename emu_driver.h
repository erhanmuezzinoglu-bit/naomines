#ifndef EMU_DRIVER_H
#define EMU_DRIVER_H

#include <stdint.h>

#include "fake6502.h"
#include "ppu.h"
#include "nes_input.h"
#include "trace.h"
#include "nes_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emülatörün başlatılması için gerekli fonksiyon
void emu_driver_init(fake6502_context *cpu,
                     ppu_t *ppu,
                     nes_input_t *in,
                     trace_state_t *tr,
                     nes_bus_t *bus);

// Reset işlemi sonrası reset vector okur
void emu_driver_reset_from_vector(void);

/* Tek adımlık CPU işleme fonksiyonu (step).
   Adım sonrası harcanan toplam CPU döngüsünü döndürür (uint32_t).
*/
uint32_t emu_driver_step_1(void);

// Toplam işlenen komut sayısını döndürür
uint32_t emu_driver_instr_count(void);

// İşlenen komut sayacını sıfırlar
void emu_driver_instr_count_reset(void);

/*
   UI için bir yardımcı fonksiyon 
   Verilen değeri ([lo, hi] arasında) sınırlar.
*/
int emu_driver_clamp_int(int v, int lo, int hi);

// IRQ tetikleme isteği (APU veya başka bir donanım çağırabilir)
void emu_driver_request_irq(void);

// IRQ pending'i iptal et (APU $4017 inhibit yazımı için — 14-3 fail M fix)
void emu_driver_clear_irq(void);

// $2000 yazımı tarafından NMI raise edildiğinde 1 instruction gecikmeli servis eder
// (AccuracyCoin 17-3 fix: "NMI is polled before write cycle of STA")
void emu_driver_request_nmi_delayed(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_DRIVER_H */