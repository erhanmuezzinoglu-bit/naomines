#ifndef DEBUG_HUD_H
#define DEBUG_HUD_H

#include <stdint.h>

#include "fake6502.h"

#ifdef __cplusplus
extern "C" {
#endif

void debug_hud_draw(
    const fake6502_context *cpu,
    int autorun,
    int steps_per_frame);

#ifdef __cplusplus
}
#endif

#endif