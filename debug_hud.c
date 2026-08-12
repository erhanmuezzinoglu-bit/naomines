#include "debug_hud.h"
#include "opc.h"
#include <naomi/video.h>
#include "debug_ui.h"
#include "nes_input.h"
#include "nes_rom.h"
#include <stdio.h>

void debug_hud_draw(const fake6502_context *cpu,
                    uint32_t instr_count,
                    const ppu_t *ppu,
                    const nes_bus_t *bus,
                    const trace_state_t *tr,
                    int autorun,
                    int steps_per_frame,
                    int trace_scroll,
                    const uint16_t *watch_addrs,
                    int watch_count)
{
    int y = 12;

    #define NEXTLINE() (y += 12)

    // 1. Durum ve Kontrol Bilgileri
    // 'halted' yerine 'error_detected' kullanıyoruz. Accuracy testlerini kesmez, sadece görsel uyarı verir.
    color_t status_color = tr->error_detected ? rgb(255, 50, 50) : rgb(255, 220, 120);
    video_draw_debug_text(20, y, status_color,
        "Auto:%s SPF:%d Trace:%s Err:%s (%s)",
        autorun ? "ON" : "OFF", steps_per_frame, 
        tr->enable ? "ON" : "OFF", 
        tr->error_detected ? "YES" : "NO",
        tr->error_detected ? trace_halt_reason_str(tr->halt_reason) : "RUNNING"); //
    NEXTLINE();

    // 2. PPU Temel Zamanlama
    video_draw_debug_text(20, y, rgb(180, 180, 255),
        "PPU F:%-5d SL:%-3d CY:%-3d STAT:%02X CTRL:%02X MASK:%02X",
        ppu->frame, ppu->scanline, ppu->cycle, ppu->reg_status, ppu->reg_ctrl, ppu->reg_mask);
    NEXTLINE();

    // --- NOKTA ATIŞI: OPEN BUS VE FETCH TAKİBİ ---
    uint8_t pc_val = bus->mem[cpu->cpu.pc]; 
    video_draw_debug_text(20, y, rgb(255, 100, 255), 
        "BUS_VAL (Floating): %02X | PC_MEM[%04X]: %02X", 
        bus->open_bus_value, cpu->cpu.pc, pc_val);
    NEXTLINE();

    // 3. LOOPY REGISTER DEBUG
    video_draw_debug_text(20, y, rgb(0, 255, 255), 
        "LOOPY v:%04X t:%04X x:%d w:%d | NT:%d CX:%d CY:%d FY:%d", 
        ppu->v, ppu->t, ppu->x, ppu->w,
        (ppu->v >> 10) & 0x3, (ppu->v & 0x1F), (ppu->v >> 5) & 0x1F, (ppu->v >> 12) & 0x7);
    NEXTLINE();

    // 4. PPU REGISTER WATCH
    video_draw_debug_text(20, y, rgb(255, 255, 0), "--- PPU REGISTER WATCH ---");
    NEXTLINE();
    
    color_t latch_color = ppu->w ? rgb(255, 100, 100) : rgb(200, 200, 200);
    
    video_draw_debug_text(20, y, rgb(200, 200, 200),
        "$2000:%02X  $2001:%02X  $2002:%02X",
        ppu->reg_ctrl, ppu->reg_mask, ppu->reg_status);
    NEXTLINE();
    
    video_draw_debug_text(20, y, latch_color,
        "$2005(Scroll): %02X, %02X | $2006(Addr): %02X, %02X (W:%d)",
        bus->watch_ppu_2005_a, bus->watch_ppu_2005_b,
        bus->watch_ppu_2006_hi, bus->watch_ppu_2006_lo, ppu->w);
    NEXTLINE();

    // 5. CPU STATUS
    video_draw_debug_text(20, y, rgb(255, 150, 50), "--- CPU STATUS ---");
    NEXTLINE();
    video_draw_debug_text(20, y, rgb(255, 255, 255),
        "PC:%04X  A:%02X  X:%02X  Y:%02X  S:%02X  P:%02X  OP:[%02X]",
        cpu->cpu.pc, cpu->cpu.a, cpu->cpu.x, cpu->cpu.y, cpu->cpu.s, cpu->cpu.flags, cpu->emu.opcode);
    NEXTLINE();

    // 6. OAM ve DMA
    video_draw_debug_text(20, y, rgb(150, 255, 150),
        "OAMADDR:%02X  DMA_COUNT:%lu  UNROM_BANK:%d",
        ppu->oam_addr, (unsigned long)bus->dbg_oam_dma_count, nes_unrom_current_bank);
    NEXTLINE();
    // --- APU DEBUG (Phase 2 tanı) ---
    video_draw_debug_text(20, y, rgb(255, 200, 100),
        "P1 en=%d lc=%d per=%03X ds=%d vol=%d  P2 en=%d lc=%d per=%03X ds=%d vol=%d",
        bus->apu.p1.enabled, bus->apu.p1.length_counter, bus->apu.p1.timer_period,
        bus->apu.p1.duty_step, bus->apu.p1.volume_or_period,
        bus->apu.p2.enabled, bus->apu.p2.length_counter, bus->apu.p2.timer_period,
        bus->apu.p2.duty_step, bus->apu.p2.volume_or_period);
    NEXTLINE();

    video_draw_debug_text(20, y, rgb(255, 200, 100),
        "$4015 last=%02X  apu_4017=%02X  fc_irq=%d",
        bus->apu.reg_4015_enable, bus->apu.reg_4017_frame_counter, bus->apu.frame_counter_irq);
    NEXTLINE();

    // Alt paneller ve diğer çizimler
    int trace_lines = 12; 
    int trace_y = 470 - (trace_lines * 18) - 20;

    // 'tr' nesnesi 'trace_state_t' olarak doğrudan gönderiliyor.
    debug_draw_trace(20, trace_y, trace_lines, trace_scroll, tr, bus->mem);
    debug_draw_watch_ram(550, trace_y, bus->mem, watch_addrs, watch_count);

    video_draw_debug_text(550, trace_y + 18, rgb(180, 180, 255), "PAD1:%02X (%c%c%c%c%c%c%c%c)",
        bus->in->pad1_shift_last,
        (bus->in->pad1_shift_last & 0x80) ? 'A' : '.', (bus->in->pad1_shift_last & 0x40) ? 'B' : '.',
        (bus->in->pad1_shift_last & 0x20) ? 'S' : '.', (bus->in->pad1_shift_last & 0x10) ? 's' : '.',
        (bus->in->pad1_shift_last & 0x08) ? 'U' : '.', (bus->in->pad1_shift_last & 0x04) ? 'D' : '.',
        (bus->in->pad1_shift_last & 0x02) ? 'L' : '.', (bus->in->pad1_shift_last & 0x01) ? 'R' : '.');

    video_draw_debug_text(20, 470, rgb(120, 200, 255),
        "UI: F=AUTORUN Z=RESET X=STP | MARIO: A=A S=B D=ST ENT=SEL ARW=DPAD");
}