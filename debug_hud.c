#include "debug_hud.h"

#include <naomi/video.h>
#include <stdint.h>

void debug_hud_draw(
    const fake6502_context *cpu,
    int autorun,
    int steps_per_frame)
{
    if (!cpu) return;

    const int x = 8;
    int y = 4;
    color_t normal = rgb(210, 210, 220);
    color_t key = rgb(150, 200, 255);

    video_draw_debug_text(
        x, y, normal,
        "Auto:%s Budget:%d",
        autorun ? "ON" : "OFF",
        steps_per_frame);
    y += 10;

    video_draw_debug_text(
        x, y, normal,
        "PC:%04X OP:%02X X:%02X CPU:%lu",
        (unsigned)cpu->cpu.pc,
        (unsigned)cpu->emu.opcode,
        (unsigned)cpu->cpu.x,
        (unsigned long)(uint32_t)cpu->emu.clockticks);

    video_draw_debug_text(
        x, 468, key,
        "B4:Auto B5:Reset B6:8 steps | Arrows:game");
}