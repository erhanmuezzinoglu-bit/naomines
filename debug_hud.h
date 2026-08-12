#ifndef DEBUG_HUD_H
#define DEBUG_HUD_H

#include <stdint.h>

#include "fake6502.h"
#include "ppu.h"
#include "trace.h"
#include "nes_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

void debug_hud_draw(const fake6502_context *cpu,
                    uint32_t instr_count,
                    const ppu_t *ppu,
                    const nes_bus_t *bus,
                    const trace_state_t *tr,
                    int autorun,
                    int steps_per_frame,
                    int trace_scroll,
                    const uint16_t *watch_addrs,
                    int watch_count);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_HUD_H */