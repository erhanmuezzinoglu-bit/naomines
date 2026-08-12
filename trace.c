#include "trace.h"
#include <string.h>
#include <stdio.h>

void trace_init(trace_state_t *t)
{
    memset(t, 0, sizeof(*t));
    t->enable = 1;
}

void trace_push(trace_state_t *t, fake6502_context *ctx,
                uint16_t addr, uint8_t val, int is_write, int unknown)
{
    // t->halted kontrolü kaldırıldı. Accuracy testleri için kayıt asla kesilmez.[cite: 5]
    if (!t->enable) return;

    trace_evt_t *e = &t->rb[t->head % TRACE_SIZE];
    
    // CPU Snapshot
    if (ctx) {
        e->pc = (uint16_t)ctx->cpu.pc;
        e->a  = ctx->cpu.a;
        e->x  = ctx->cpu.x;
        e->y  = ctx->cpu.y;
        e->p  = ctx->cpu.flags;
        e->sp = ctx->cpu.s;
        e->opcode = ctx->emu.opcode; // O an işlenen opcode
    } else {
        memset(e, 0, sizeof(trace_evt_t));
    }

    // Bus/IO Snapshot
    e->addr = addr;
    e->val = val;
    e->is_write = (uint8_t)(is_write ? 1 : 0);
    e->unknown = (uint8_t)(unknown ? 1 : 0);

    t->head++;
}

void trace_halt_now(trace_state_t *t, fake6502_context *ctx,
                    halt_reason_t r, uint16_t addr, uint8_t val)
{
    // Sistem artık "donmaz" (t->halted = 1 yapılmaz), sadece hata durumu not edilir.[cite: 5]
    t->error_detected = 1; 
    t->halt_reason = r;
    t->halt_pc = ctx ? (uint16_t)ctx->cpu.pc : 0;
    t->halt_addr = addr;
    t->halt_val = val;
    
    // Konsola basılan mesaj "HALT" yerine "ALERT" olarak güncellendi.
    printf("\n[ALERT] Accuracy Issue: %s at PC: $%04X, Addr: $%04X, Val: $%02X\n", 
           trace_halt_reason_str(r), t->halt_pc, addr, val);
}

const char *trace_halt_reason_str(halt_reason_t r)
{
    switch (r) {
        case HALT_NONE:               return "NONE";
        case HALT_UNKNOWN_IO_READ:    return "UNKNOWN_IO_READ";
        case HALT_UNKNOWN_IO_WRITE:   return "UNKNOWN_IO_WRITE";
        case HALT_ILLEGAL_OPCODE:     return "ILLEGAL_OPCODE";
        case HALT_STACK_OVERFLOW:     return "STACK_OVERFLOW";
        case HALT_PPU_SYNC_ERROR:     return "PPU_SYNC_ERROR";
        case HALT_SPRITE_0_TIMEOUT:   return "SPRITE_0_TIMEOUT";
        default:                      return "UNKNOWN_REASON";
    }
}