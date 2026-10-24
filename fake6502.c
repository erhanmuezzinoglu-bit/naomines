#define NMOS6502
#include "fake6502.h"
#include <stdbool.h>
#include <stdint.h>

extern int g_nmi_ready;
extern int g_irq_pending;
extern int g_nmi_pending;
extern int g_irq_poll_snapshot;

// -------------------------------------------------------------------
// Yardımcı Fonksiyonlar
// -------------------------------------------------------------------

void fake6502_push_8(fake6502_context *c, uint8_t pushval) {
    fake6502_mem_write(c, FAKE6502_STACK_BASE + c->cpu.s--, pushval);
}

void fake6502_push_16(fake6502_context *c, uint16_t pushval) {
    fake6502_push_8(c, (pushval >> 8) & 0xFF);
    fake6502_push_8(c, pushval & 0xFF);
}

uint8_t fake6502_pull_8(fake6502_context *c) {
    return(fake6502_mem_read(c, FAKE6502_STACK_BASE + ++c->cpu.s));
}

uint16_t fake6502_pull_16(fake6502_context *c) {
    uint16_t low = fake6502_pull_8(c);
    uint16_t high = fake6502_pull_8(c);
    return (high << 8) | low;
}

static uint16_t fake6502_mem_read16(fake6502_context *c, uint16_t addr) {
    uint16_t low = fake6502_mem_read(c, addr);
    uint16_t high = fake6502_mem_read(c, addr + 1);
    return low | (high << 8);
}

// -------------------------------------------------------------------
// RDY/DMC DMA gözlemi: kararsız store talimatları
// -------------------------------------------------------------------

static uint8_t s_unstable_store_dma_halt = 0;

static bool fake6502_is_unstable_store_opcode(uint8_t opcode)
{
    return opcode == 0x93 || opcode == 0x9B || opcode == 0x9C ||
           opcode == 0x9E || opcode == 0x9F;
}

static uint8_t fake6502_unstable_store_timing_read(
    fake6502_context *c,
    uint16_t addr
)
{
    if (!fake6502_is_unstable_store_opcode(c->emu.opcode)) {
        return fake6502_mem_read(c, addr);
    }

    uint32_t before = (uint32_t)c->emu.clockticks;
    uint8_t value = fake6502_mem_read(c, addr);
    uint32_t elapsed = (uint32_t)c->emu.clockticks - before;

    if (elapsed > 1u) {
        s_unstable_store_dma_halt = 1;
    }

    return value;
}

// -------------------------------------------------------------------
// Adresleme Modları
// -------------------------------------------------------------------

FAKE6502_FN_ADDR_MODE(imp) {
    fake6502_mem_read(c, c->cpu.pc);
}

FAKE6502_FN_ADDR_MODE(acc) {
    fake6502_mem_read(c, c->cpu.pc);
}

FAKE6502_FN_ADDR_MODE(imm) {
    c->emu.ea = c->cpu.pc++;
}

FAKE6502_FN_ADDR_MODE(zp) {
    c->emu.ea =
        (uint16_t)fake6502_mem_read(
            c,
            (uint16_t)c->cpu.pc++
        );
}

FAKE6502_FN_ADDR_MODE(zpx) {
    uint16_t zp_addr =
        (uint16_t)fake6502_mem_read(
            c,
            (uint16_t)c->cpu.pc++
        );

    fake6502_mem_read(c, zp_addr);

    c->emu.ea =
        (zp_addr + (uint16_t)c->cpu.x) & 0xFF;
}

FAKE6502_FN_ADDR_MODE(zpy) {
    uint16_t zp_addr =
        (uint16_t)fake6502_mem_read(
            c,
            (uint16_t)c->cpu.pc++
        );

    fake6502_mem_read(c, zp_addr);

    c->emu.ea =
        (zp_addr + (uint16_t)c->cpu.y) & 0xFF;
}

FAKE6502_FN_ADDR_MODE(rel) {
    uint16_t rel =
        (uint16_t)fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    if (rel & 0x80)
        rel |= 0xFF00;

    c->emu.ea = c->cpu.pc + rel;
}

FAKE6502_FN_ADDR_MODE(abso) {
    c->emu.ea =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    c->cpu.pc += 2;
}

FAKE6502_FN_ADDR_MODE(absx) {
    uint16_t base =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    c->cpu.pc += 2;

    uint16_t dummy_addr =
        (base & 0xFF00) |
        ((base + c->cpu.x) & 0x00FF);

    fake6502_unstable_store_timing_read(c, dummy_addr);

    c->emu.ea =
        base + (uint16_t)c->cpu.x;
}

FAKE6502_FN_ADDR_MODE(absx_p) {
    uint16_t base =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    c->cpu.pc += 2;

    c->emu.ea =
        base + (uint16_t)c->cpu.x;

    if ((base & 0xFF00) !=
        (c->emu.ea & 0xFF00)) {

        uint16_t dummy_addr =
            (base & 0xFF00) |
            (c->emu.ea & 0x00FF);

        fake6502_mem_read(c, dummy_addr);
    }
}

FAKE6502_FN_ADDR_MODE(absy) {
    uint16_t base =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    c->cpu.pc += 2;

    uint16_t dummy_addr =
        (base & 0xFF00) |
        ((base + c->cpu.y) & 0x00FF);

    fake6502_unstable_store_timing_read(c, dummy_addr);

    c->emu.ea =
        base + (uint16_t)c->cpu.y;
}

FAKE6502_FN_ADDR_MODE(absy_p) {
    uint16_t base =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    c->cpu.pc += 2;

    c->emu.ea =
        base + (uint16_t)c->cpu.y;

    if ((base & 0xFF00) !=
        (c->emu.ea & 0xFF00)) {

        uint16_t dummy_addr =
            (base & 0xFF00) |
            (c->emu.ea & 0x00FF);

        fake6502_mem_read(c, dummy_addr);
    }
}

FAKE6502_FN_ADDR_MODE(ind) {
    uint16_t eahelp =
        fake6502_mem_read16(
            c,
            c->cpu.pc
        );

    uint16_t eahelp2 =
        (eahelp & 0xFF00) |
        ((eahelp + 1) & 0x00FF);

    uint16_t low =
        fake6502_mem_read(c, eahelp);

    uint16_t high =
        fake6502_mem_read(c, eahelp2);

    c->emu.ea =
        low | (high << 8);

    c->cpu.pc += 2;
}

FAKE6502_FN_ADDR_MODE(indx) {
    uint16_t ptr =
        (uint16_t)fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    fake6502_mem_read(c, ptr);

    uint16_t eahelp =
        (ptr + (uint16_t)c->cpu.x) & 0xFF;

    uint16_t low =
        fake6502_mem_read(c, eahelp);

    uint16_t high =
        fake6502_mem_read(
            c,
            (eahelp + 1) & 0xFF
        );

    c->emu.ea =
        low | (high << 8);
}

FAKE6502_FN_ADDR_MODE(indy) {
    uint16_t ptr =
        (uint16_t)fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    uint16_t ptr_low =
        fake6502_mem_read(c, ptr);

    uint16_t ptr_high =
        fake6502_mem_read(
            c,
            (ptr + 1) & 0xFF
        );

    uint16_t base =
        ptr_low | (ptr_high << 8);

    uint16_t dummy_addr =
        (base & 0xFF00) |
        ((base + c->cpu.y) & 0x00FF);

    fake6502_unstable_store_timing_read(c, dummy_addr);

    c->emu.ea =
        base + (uint16_t)c->cpu.y;
}

FAKE6502_FN_ADDR_MODE(indy_p) {
    uint16_t ptr =
        (uint16_t)fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    uint16_t ptr_low =
        fake6502_mem_read(c, ptr);

    uint16_t ptr_high =
        fake6502_mem_read(
            c,
            (ptr + 1) & 0xFF
        );

    uint16_t base =
        ptr_low | (ptr_high << 8);

    c->emu.ea =
        base + (uint16_t)c->cpu.y;

    if ((base & 0xFF00) !=
        (c->emu.ea & 0xFF00)) {

        uint16_t dummy_addr =
            (base & 0xFF00) |
            (c->emu.ea & 0x00FF);

        fake6502_mem_read(c, dummy_addr);
    }
}

// -------------------------------------------------------------------
// ALU ve Operasyonlar
// -------------------------------------------------------------------

static uint8_t s_rmw_val = 0;

uint16_t fake6502_get_value(fake6502_context *c) {
    if (fake6502_opcodes[c->emu.opcode].addr_mode == acc)
        return((uint16_t)c->cpu.a);

    uint8_t v =
        fake6502_mem_read(
            c,
            c->emu.ea
        );

    s_rmw_val = v;

    return (uint16_t)v;
}

void fake6502_put_value(
    fake6502_context *c,
    uint16_t saveval
) {
    if (fake6502_opcodes[c->emu.opcode].addr_mode == acc) {

        c->cpu.a =
            (uint8_t)(saveval & 0x00FF);

    } else {

        fake6502_mem_write(
            c,
            c->emu.ea,
            s_rmw_val
        );

        fake6502_mem_write(
            c,
            c->emu.ea,
            (uint8_t)(saveval)
        );
    }
}

void fake6502_compare_val(
    fake6502_context *c,
    uint16_t r,
    uint8_t value
) {
    uint16_t result = r - value;

    if (r >= value)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);

    if (r == value)
        fake6502_zero_set(c);
    else
        fake6502_zero_clear(c);

    fake6502_sign_calc(c, result);
}

static uint8_t add8(
    fake6502_context *c,
    uint16_t a,
    uint16_t b,
    bool carry
) {
    uint16_t result =
        a + b + (uint16_t)(carry ? 1 : 0);

    fake6502_zero_calc(c, result);
    fake6502_overflow_calc(c, result, a, b);
    fake6502_sign_calc(c, result);
    fake6502_carry_calc(c, result);

    return(result);
}

static uint8_t rotate_right(
    fake6502_context *c,
    uint16_t value
) {
    uint16_t result =
        (value >> 1) |
        ((c->cpu.flags & FAKE6502_CARRY_FLAG) << 7);

    if (value & 1)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t rotate_left(
    fake6502_context *c,
    uint16_t value
) {
    uint16_t result =
        (value << 1) |
        (c->cpu.flags & FAKE6502_CARRY_FLAG);

    fake6502_carry_calc(c, result);
    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t logical_shift_right(
    fake6502_context *c,
    uint8_t value
) {
    uint16_t result =
        value >> 1;

    if (value & 1)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t arithmetic_shift_left(
    fake6502_context *c,
    uint8_t value
) {
    uint16_t result =
        value << 1;

    fake6502_carry_calc(c, result);
    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t boolean_and(
    fake6502_context *c,
    uint8_t a,
    uint8_t b
) {
    uint16_t result =
        (uint16_t)a & b;

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t exclusive_or(
    fake6502_context *c,
    uint8_t a,
    uint8_t b
) {
    uint16_t result =
        a ^ b;

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t increment(
    fake6502_context *c,
    uint8_t r
) {
    uint16_t result =
        r + 1;

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

static uint8_t decrement(
    fake6502_context *c,
    uint8_t r
) {
    uint16_t result =
        r - 1;

    fake6502_zero_calc(c, result);
    fake6502_sign_calc(c, result);

    return(result);
}

// -------------------------------------------------------------------
// Opcodes
// -------------------------------------------------------------------

FAKE6502_FN_OPCODE(adc) {
    fake6502_accum_save(
        c,
        add8(
            c,
            c->cpu.a,
            fake6502_get_value(c),
            c->cpu.flags & FAKE6502_CARRY_FLAG
        )
    );
}

FAKE6502_FN_OPCODE(and) {
    fake6502_accum_save(
        c,
        boolean_and(
            c,
            c->cpu.a,
            fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(asl) {
    fake6502_put_value(
        c,
        arithmetic_shift_left(
            c,
            fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(bra) {
    uint16_t oldpc =
        c->cpu.pc;

    c->cpu.pc =
        c->emu.ea;

    c->emu.branch_taken = 1;

    fake6502_mem_read(c, oldpc);

    if ((oldpc & 0xFF00) !=
        (c->cpu.pc & 0xFF00)) {

        c->emu.branch_page_crossed = 1;

        c->emu.branch_irq_second_poll =
            g_irq_pending ? 1 : 0;

        fake6502_mem_read(
            c,
            (oldpc & 0xFF00) |
            (c->cpu.pc & 0x00FF)
        );
    }
}

FAKE6502_FN_OPCODE(bcc) {
    if ((c->cpu.flags & FAKE6502_CARRY_FLAG) == 0)
        bra(c);
}

FAKE6502_FN_OPCODE(bcs) {
    if ((c->cpu.flags & FAKE6502_CARRY_FLAG) ==
        FAKE6502_CARRY_FLAG)
        bra(c);
}

FAKE6502_FN_OPCODE(beq) {
    if ((c->cpu.flags & FAKE6502_ZERO_FLAG) ==
        FAKE6502_ZERO_FLAG)
        bra(c);
}

FAKE6502_FN_OPCODE(bmi) {
    if ((c->cpu.flags & FAKE6502_SIGN_FLAG) ==
        FAKE6502_SIGN_FLAG)
        bra(c);
}

FAKE6502_FN_OPCODE(bne) {
    if ((c->cpu.flags & FAKE6502_ZERO_FLAG) == 0)
        bra(c);
}

FAKE6502_FN_OPCODE(bpl) {
    if ((c->cpu.flags & FAKE6502_SIGN_FLAG) == 0)
        bra(c);
}

FAKE6502_FN_OPCODE(bvc) {
    if ((c->cpu.flags & FAKE6502_OVERFLOW_FLAG) == 0)
        bra(c);
}

FAKE6502_FN_OPCODE(bvs) {
    if ((c->cpu.flags & FAKE6502_OVERFLOW_FLAG) ==
        FAKE6502_OVERFLOW_FLAG)
        bra(c);
}

FAKE6502_FN_OPCODE(bit) {
    uint8_t value =
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        (uint16_t)c->cpu.a & value
    );

    c->cpu.flags =
        (c->cpu.flags & 0x3F) |
        (value & 0xC0);
}

FAKE6502_FN_OPCODE(brk)
{
    c->cpu.pc++;

    int nmi_hijack = g_nmi_pending ? 1 : 0;

    fake6502_push_8(
        c,
        (c->cpu.pc >> 8) & 0xFF
    );

    if (g_nmi_pending) {
        nmi_hijack = 1;
    }

    fake6502_push_8(
        c,
        c->cpu.pc & 0xFF
    );

    if (g_nmi_pending) {
        nmi_hijack = 1;
    }

    uint8_t pushed_flags =
        (uint8_t)(c->cpu.flags | 0x30);

    fake6502_push_8(
        c,
        pushed_flags
    );

    fake6502_interrupt_set(c);

    if (g_nmi_pending) {
        nmi_hijack = 1;
    }

    if (nmi_hijack) {

        c->cpu.pc =
            fake6502_mem_read16(
                c,
                0xFFFA
            );

        g_nmi_pending = 0;
        c->emu.nmi_defer_after_interrupt = 0;

    } else {

        c->cpu.pc =
            fake6502_mem_read16(
                c,
                0xFFFE
            );

        if (g_nmi_pending) {
            c->emu.nmi_defer_after_interrupt = 1;
        }
    }
}

FAKE6502_FN_OPCODE(clc) {
    fake6502_carry_clear(c);
}

FAKE6502_FN_OPCODE(cld) {
    fake6502_decimal_clear(c);
}

FAKE6502_FN_OPCODE(cli) {
    fake6502_interrupt_clear(c);
}

FAKE6502_FN_OPCODE(clv) {
    fake6502_overflow_clear(c);
}

FAKE6502_FN_OPCODE(cmp) {
    fake6502_compare_val(
        c,
        c->cpu.a,
        (uint8_t)fake6502_get_value(c)
    );
}

FAKE6502_FN_OPCODE(cpx) {
    fake6502_compare_val(
        c,
        c->cpu.x,
        (uint8_t)fake6502_get_value(c)
    );
}

FAKE6502_FN_OPCODE(cpy) {
    fake6502_compare_val(
        c,
        c->cpu.y,
        (uint8_t)fake6502_get_value(c)
    );
}

FAKE6502_FN_OPCODE(dec) {
    fake6502_put_value(
        c,
        decrement(
            c,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(dex) {
    c->cpu.x =
        decrement(
            c,
            c->cpu.x
        );
}

FAKE6502_FN_OPCODE(dey) {
    c->cpu.y =
        decrement(
            c,
            c->cpu.y
        );
}

FAKE6502_FN_OPCODE(eor) {
    fake6502_accum_save(
        c,
        exclusive_or(
            c,
            c->cpu.a,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(inc) {
    fake6502_put_value(
        c,
        increment(
            c,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(inx) {
    c->cpu.x =
        increment(
            c,
            c->cpu.x
        );
}

FAKE6502_FN_OPCODE(iny) {
    c->cpu.y =
        increment(
            c,
            c->cpu.y
        );
}

FAKE6502_FN_OPCODE(jmp) {
    c->cpu.pc =
        c->emu.ea;
}

FAKE6502_FN_ADDR_MODE(none) {
}

FAKE6502_FN_OPCODE(jsr) {
    uint8_t low =
        fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    fake6502_mem_read(
        c,
        FAKE6502_STACK_BASE + c->cpu.s
    );

    fake6502_push_8(
        c,
        (c->cpu.pc >> 8) & 0xFF
    );

    fake6502_push_8(
        c,
        c->cpu.pc & 0xFF
    );

    uint8_t high =
        fake6502_mem_read(
            c,
            c->cpu.pc
        );

    c->cpu.pc =
        (high << 8) | low;
}

FAKE6502_FN_OPCODE(lda) {
    c->cpu.a =
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(ldx) {
    c->cpu.x =
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.x
    );

    fake6502_sign_calc(
        c,
        c->cpu.x
    );
}

FAKE6502_FN_OPCODE(ldy) {
    c->cpu.y =
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.y
    );

    fake6502_sign_calc(
        c,
        c->cpu.y
    );
}

FAKE6502_FN_OPCODE(lsr) {
    fake6502_put_value(
        c,
        logical_shift_right(
            c,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(nop) {
    if (fake6502_opcodes[c->emu.opcode].addr_mode != imp &&
        fake6502_opcodes[c->emu.opcode].addr_mode != acc) {

        fake6502_mem_read(
            c,
            c->emu.ea
        );
    }
}

FAKE6502_FN_OPCODE(ora) {
    c->cpu.a |=
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(pha) {
    fake6502_push_8(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(php) {
    fake6502_push_8(
        c,
        c->cpu.flags | 0x30
    );
}

FAKE6502_FN_OPCODE(pla) {
    c->emu.clockticks++;

    c->cpu.a =
        fake6502_pull_8(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(plp) {
    c->emu.clockticks++;

    c->cpu.flags =
        (fake6502_pull_8(c) & ~0x10) | 0x20;
}

FAKE6502_FN_OPCODE(rol) {
    fake6502_put_value(
        c,
        rotate_left(
            c,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(ror) {
    fake6502_put_value(
        c,
        rotate_right(
            c,
            (uint8_t)fake6502_get_value(c)
        )
    );
}

FAKE6502_FN_OPCODE(rti) {
    c->emu.clockticks++;

    c->cpu.flags =
        (fake6502_pull_8(c) & ~0x10) | 0x20;

    c->cpu.pc =
        fake6502_pull_16(c);
}

FAKE6502_FN_OPCODE(rts) {
    c->emu.clockticks++;

    c->cpu.pc =
        fake6502_pull_16(c);

    fake6502_mem_read(
        c,
        c->cpu.pc
    );

    c->cpu.pc++;
}

FAKE6502_FN_OPCODE(sbc) {
    fake6502_accum_save(
        c,
        add8(
            c,
            c->cpu.a,
            (uint8_t)fake6502_get_value(c) ^ 0x00FF,
            c->cpu.flags & FAKE6502_CARRY_FLAG
        )
    );
}

FAKE6502_FN_OPCODE(sec) {
    fake6502_carry_set(c);
}

FAKE6502_FN_OPCODE(sed) {
    fake6502_decimal_set(c);
}

FAKE6502_FN_OPCODE(sei) {
    fake6502_interrupt_set(c);
}

FAKE6502_FN_OPCODE(sta) {
    fake6502_mem_write(
        c,
        c->emu.ea,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(stx) {
    fake6502_mem_write(
        c,
        c->emu.ea,
        c->cpu.x
    );
}

FAKE6502_FN_OPCODE(sty) {
    fake6502_mem_write(
        c,
        c->emu.ea,
        c->cpu.y
    );
}

FAKE6502_FN_OPCODE(tax) {
    c->cpu.x =
        c->cpu.a;

    fake6502_zero_calc(
        c,
        c->cpu.x
    );

    fake6502_sign_calc(
        c,
        c->cpu.x
    );
}

FAKE6502_FN_OPCODE(tay) {
    c->cpu.y =
        c->cpu.a;

    fake6502_zero_calc(
        c,
        c->cpu.y
    );

    fake6502_sign_calc(
        c,
        c->cpu.y
    );
}

FAKE6502_FN_OPCODE(tsx) {
    c->cpu.x =
        c->cpu.s;

    fake6502_zero_calc(
        c,
        c->cpu.x
    );

    fake6502_sign_calc(
        c,
        c->cpu.x
    );
}

FAKE6502_FN_OPCODE(txa) {
    c->cpu.a =
        c->cpu.x;

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(txs) {
    c->cpu.s =
        c->cpu.x;
}

FAKE6502_FN_OPCODE(tya) {
    c->cpu.a =
        c->cpu.y;

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

// -------------------------------------------------------------------
// Illegal Opcodes
// -------------------------------------------------------------------

FAKE6502_FN_OPCODE(lax) {
    c->cpu.a =
        c->cpu.x =
            (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(sax) {
    fake6502_mem_write(
        c,
        c->emu.ea,
        c->cpu.a & c->cpu.x
    );
}

FAKE6502_FN_OPCODE(dcp) {
    uint8_t val =
        decrement(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    fake6502_compare_val(
        c,
        c->cpu.a,
        val
    );
}

FAKE6502_FN_OPCODE(isb) {
    uint8_t val =
        increment(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    fake6502_accum_save(
        c,
        add8(
            c,
            c->cpu.a,
            val ^ 0x00FF,
            c->cpu.flags & FAKE6502_CARRY_FLAG
        )
    );
}

FAKE6502_FN_OPCODE(slo) {
    uint8_t val =
        arithmetic_shift_left(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    c->cpu.a |= val;

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(rla) {
    uint8_t val =
        rotate_left(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    c->cpu.a &= val;

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(sre) {
    uint8_t val =
        logical_shift_right(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    c->cpu.a ^= val;

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(rra) {
    uint8_t val =
        rotate_right(
            c,
            (uint8_t)fake6502_get_value(c)
        );

    fake6502_put_value(
        c,
        val
    );

    fake6502_accum_save(
        c,
        add8(
            c,
            c->cpu.a,
            val,
            c->cpu.flags & FAKE6502_CARRY_FLAG
        )
    );
}

FAKE6502_FN_OPCODE(anc) {
    fake6502_accum_save(
        c,
        boolean_and(
            c,
            c->cpu.a,
            (uint8_t)fake6502_get_value(c)
        )
    );

    if (c->cpu.a & 0x80)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);
}

FAKE6502_FN_OPCODE(asr) {
    uint8_t val =
        boolean_and(
            c,
            c->cpu.a,
            (uint8_t)fake6502_get_value(c)
        );

    c->cpu.a =
        logical_shift_right(
            c,
            val
        );
}

FAKE6502_FN_OPCODE(arr) {
    uint8_t val =
        (uint8_t)fake6502_get_value(c) &
        c->cpu.a;

    uint8_t res =
        (val >> 1) |
        ((c->cpu.flags & FAKE6502_CARRY_FLAG) << 7);

    fake6502_zero_calc(c, res);
    fake6502_sign_calc(c, res);

    if (res & 0x40)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);

    if (((res >> 6) ^ (res >> 5)) & 1)
        fake6502_overflow_set(c);
    else
        fake6502_overflow_clear(c);

    c->cpu.a =
        res;
}

FAKE6502_FN_OPCODE(ane) {
    c->cpu.a =
        (c->cpu.a | 0xEE) &
        c->cpu.x &
        (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(lxa) {
    c->cpu.a =
        c->cpu.x =
            (uint8_t)fake6502_get_value(c);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

FAKE6502_FN_OPCODE(axs) {
    uint8_t val =
        (uint8_t)fake6502_get_value(c);

    uint8_t res =
        (c->cpu.a & c->cpu.x) - val;

    if ((c->cpu.a & c->cpu.x) >= val)
        fake6502_carry_set(c);
    else
        fake6502_carry_clear(c);

    fake6502_zero_calc(c, res);
    fake6502_sign_calc(c, res);

    c->cpu.x =
        res;
}

// -------------------------------------------------------------------
// Unstable store helper
// -------------------------------------------------------------------

static uint16_t fake6502_unstable_store_addr(
    fake6502_context *c,
    uint8_t index,
    uint8_t high_mask
)
{
    uint16_t base =
        (uint16_t)(
            c->emu.ea -
            (uint16_t)index
        );

    uint8_t h =
        (uint8_t)(
            ((base >> 8) + 1) & 0xFF
        );

    if ((base & 0xFF00) ==
        (c->emu.ea & 0xFF00)) {

        return c->emu.ea;
    }

    uint8_t corrupted_high =
        h & high_mask;

    return
        ((uint16_t)corrupted_high << 8) |
        (c->emu.ea & 0x00FF);
}

FAKE6502_FN_OPCODE(shx) {
    if (s_unstable_store_dma_halt) {
        fake6502_mem_write(c, c->emu.ea, c->cpu.x);
        return;
    }

    uint8_t h;

    uint16_t base =
        (uint16_t)(
            c->emu.ea -
            (uint16_t)c->cpu.y
        );

    h =
        (uint8_t)(
            ((base >> 8) + 1) & 0xFF
        );

    uint16_t addr =
        fake6502_unstable_store_addr(
            c,
            c->cpu.y,
            c->cpu.x
        );

    uint8_t value =
        (uint8_t)(
            c->cpu.x & h
        );

    fake6502_mem_write(
        c,
        addr,
        value
    );
}

FAKE6502_FN_OPCODE(shy) {
    if (s_unstable_store_dma_halt) {
        fake6502_mem_write(c, c->emu.ea, c->cpu.y);
        return;
    }

    uint8_t h;

    uint16_t base =
        (uint16_t)(
            c->emu.ea -
            (uint16_t)c->cpu.x
        );

    h =
        (uint8_t)(
            ((base >> 8) + 1) & 0xFF
        );

    uint16_t addr =
        fake6502_unstable_store_addr(
            c,
            c->cpu.x,
            c->cpu.y
        );

    uint8_t value =
        (uint8_t)(
            c->cpu.y & h
        );

    fake6502_mem_write(
        c,
        addr,
        value
    );
}

FAKE6502_FN_OPCODE(sha) {
    if (s_unstable_store_dma_halt) {
        fake6502_mem_write(
            c,
            c->emu.ea,
            (uint8_t)(c->cpu.a & c->cpu.x)
        );
        return;
    }

    uint16_t base =
        (uint16_t)(
            c->emu.ea -
            (uint16_t)c->cpu.y
        );

    uint8_t h =
        (uint8_t)(
            ((base >> 8) + 1) & 0xFF
        );

    uint16_t addr =
        fake6502_unstable_store_addr(
            c,
            c->cpu.y,
            (uint8_t)(
                c->cpu.a & c->cpu.x
            )
        );

    uint8_t value =
        (uint8_t)(
            c->cpu.a &
            c->cpu.x &
            h
        );

    fake6502_mem_write(
        c,
        addr,
        value
    );
}

FAKE6502_FN_OPCODE(shs) {
    uint8_t new_s =
        (uint8_t)(
            c->cpu.a &
            c->cpu.x
        );

    c->cpu.s =
        new_s;

    if (s_unstable_store_dma_halt) {
        fake6502_mem_write(c, c->emu.ea, new_s);
        return;
    }

    uint16_t base =
        (uint16_t)(
            c->emu.ea -
            (uint16_t)c->cpu.y
        );

    uint8_t h =
        (uint8_t)(
            ((base >> 8) + 1) & 0xFF
        );

    uint16_t addr =
        fake6502_unstable_store_addr(
            c,
            c->cpu.y,
            new_s
        );

    uint8_t value =
        (uint8_t)(
            new_s & h
        );

    fake6502_mem_write(
        c,
        addr,
        value
    );
}

FAKE6502_FN_OPCODE(lae) {
    uint8_t val =
        (uint8_t)fake6502_get_value(c);

    c->cpu.a =
        c->cpu.x =
        c->cpu.s =
            (val & c->cpu.s);

    fake6502_zero_calc(
        c,
        c->cpu.a
    );

    fake6502_sign_calc(
        c,
        c->cpu.a
    );
}

// -------------------------------------------------------------------
// Opcode Tablosu
// -------------------------------------------------------------------

fake6502_opcode fake6502_opcodes[256] = {
    /* 00 */
    {imp, brk, 7},
    {indx, ora, 6},
    {imp, nop, 2},
    {indx, slo, 8},
    {zp, nop, 3},
    {zp, ora, 3},
    {zp, asl, 5},
    {zp, slo, 5},
    {imp, php, 3},
    {imm, ora, 2},
    {acc, asl, 2},
    {imm, anc, 2},
    {abso, nop, 4},
    {abso, ora, 4},
    {abso, asl, 6},
    {abso, slo, 6},

    /* 10 */
    {rel, bpl, 2},
    {indy_p, ora, 5},
    {imp, nop, 2},
    {indy, slo, 8},
    {zpx, nop, 4},
    {zpx, ora, 4},
    {zpx, asl, 6},
    {zpx, slo, 6},
    {imp, clc, 2},
    {absy_p, ora, 4},
    {imp, nop, 2},
    {absy, slo, 7},
    {absx_p, nop, 4},
    {absx_p, ora, 4},
    {absx, asl, 7},
    {absx, slo, 7},

    /* 20 */
    {none, jsr, 6},
    {indx, and, 6},
    {imp, nop, 2},
    {indx, rla, 8},
    {zp, bit, 3},
    {zp, and, 3},
    {zp, rol, 5},
    {zp, rla, 5},
    {imp, plp, 4},
    {imm, and, 2},
    {acc, rol, 2},
    {imm, anc, 2},
    {abso, bit, 4},
    {abso, and, 4},
    {abso, rol, 6},
    {abso, rla, 6},

    /* 30 */
    {rel, bmi, 2},
    {indy_p, and, 5},
    {imp, nop, 2},
    {indy, rla, 8},
    {zpx, nop, 4},
    {zpx, and, 4},
    {zpx, rol, 6},
    {zpx, rla, 6},
    {imp, sec, 2},
    {absy_p, and, 4},
    {imp, nop, 2},
    {absy, rla, 7},
    {absx_p, nop, 4},
    {absx_p, and, 4},
    {absx, rol, 7},
    {absx, rla, 7},

    /* 40 */
    {imp, rti, 6},
    {indx, eor, 6},
    {imp, nop, 2},
    {indx, sre, 8},
    {zp, nop, 3},
    {zp, eor, 3},
    {zp, lsr, 5},
    {zp, sre, 5},
    {imp, pha, 3},
    {imm, eor, 2},
    {acc, lsr, 2},
    {imm, asr, 2},
    {abso, jmp, 3},
    {abso, eor, 4},
    {abso, lsr, 6},
    {abso, sre, 6},

    /* 50 */
    {rel, bvc, 2},
    {indy_p, eor, 5},
    {imp, nop, 2},
    {indy, sre, 8},
    {zpx, nop, 4},
    {zpx, eor, 4},
    {zpx, lsr, 6},
    {zpx, sre, 6},
    {imp, cli, 2},
    {absy_p, eor, 4},
    {imp, nop, 2},
    {absy, sre, 7},
    {absx_p, nop, 4},
    {absx_p, eor, 4},
    {absx, lsr, 7},
    {absx, sre, 7},

    /* 60 */
    {imp, rts, 6},
    {indx, adc, 6},
    {imp, nop, 2},
    {indx, rra, 8},
    {zp, nop, 3},
    {zp, adc, 3},
    {zp, ror, 5},
    {zp, rra, 5},
    {imp, pla, 4},
    {imm, adc, 2},
    {acc, ror, 2},
    {imm, arr, 2},
    {ind, jmp, 5},
    {abso, adc, 4},
    {abso, ror, 6},
    {abso, rra, 6},

    /* 70 */
    {rel, bvs, 2},
    {indy_p, adc, 5},
    {imp, nop, 2},
    {indy, rra, 8},
    {zpx, nop, 4},
    {zpx, adc, 4},
    {zpx, ror, 6},
    {zpx, rra, 6},
    {imp, sei, 2},
    {absy_p, adc, 4},
    {imp, nop, 2},
    {absy, rra, 7},
    {absx_p, nop, 4},
    {absx_p, adc, 4},
    {absx, ror, 7},
    {absx, rra, 7},

    /* 80 */
    {imm, nop, 2},
    {indx, sta, 6},
    {imm, nop, 2},
    {indx, sax, 6},
    {zp, sty, 3},
    {zp, sta, 3},
    {zp, stx, 3},
    {zp, sax, 3},
    {imp, dey, 2},
    {imm, nop, 2},
    {imp, txa, 2},
    {imm, ane, 2},
    {abso, sty, 4},
    {abso, sta, 4},
    {abso, stx, 4},
    {abso, sax, 4},

    /* 90 */
    {rel, bcc, 2},
    {indy, sta, 6},
    {imp, nop, 2},
    {indy, sha, 6},
    {zpx, sty, 4},
    {zpx, sta, 4},
    {zpy, stx, 4},
    {zpy, sax, 4},
    {imp, tya, 2},
    {absy, sta, 5},
    {imp, txs, 2},
    {absy, shs, 5},
    {absx, shy, 5},
    {absx, sta, 5},
    {absy, shx, 5},
    {absy, sha, 5},

    /* A0 */
    {imm, ldy, 2},
    {indx, lda, 6},
    {imm, ldx, 2},
    {indx, lax, 6},
    {zp, ldy, 3},
    {zp, lda, 3},
    {zp, ldx, 3},
    {zp, lax, 3},
    {imp, tay, 2},
    {imm, lda, 2},
    {imp, tax, 2},
    {imm, lxa, 2},
    {abso, ldy, 4},
    {abso, lda, 4},
    {abso, ldx, 4},
    {abso, lax, 4},

    /* B0 */
    {rel, bcs, 2},
    {indy_p, lda, 5},
    {imp, nop, 2},
    {indy_p, lax, 5},
    {zpx, ldy, 4},
    {zpx, lda, 4},
    {zpy, ldx, 4},
    {zpy, lax, 4},
    {imp, clv, 2},
    {absy_p, lda, 4},
    {imp, tsx, 2},
    {absy_p, lae, 4},
    {absx_p, ldy, 4},
    {absx_p, lda, 4},
    {absy_p, ldx, 4},
    {absy_p, lax, 4},

    /* C0 */
    {imm, cpy, 2},
    {indx, cmp, 6},
    {imm, nop, 2},
    {indx, dcp, 8},
    {zp, cpy, 3},
    {zp, cmp, 3},
    {zp, dec, 5},
    {zp, dcp, 5},
    {imp, iny, 2},
    {imm, cmp, 2},
    {imp, dex, 2},
    {imm, axs, 2},
    {abso, cpy, 4},
    {abso, cmp, 4},
    {abso, dec, 6},
    {abso, dcp, 6},

    /* D0 */
    {rel, bne, 2},
    {indy_p, cmp, 5},
    {imp, nop, 2},
    {indy, dcp, 8},
    {zpx, nop, 4},
    {zpx, cmp, 4},
    {zpx, dec, 6},
    {zpx, dcp, 6},
    {imp, cld, 2},
    {absy_p, cmp, 4},
    {imp, nop, 2},
    {absy, dcp, 7},
    {absx_p, nop, 4},
    {absx_p, cmp, 4},
    {absx, dec, 7},
    {absx, dcp, 7},

    /* E0 */
    {imm, cpx, 2},
    {indx, sbc, 6},
    {imm, nop, 2},
    {indx, isb, 8},
    {zp, cpx, 3},
    {zp, sbc, 3},
    {zp, inc, 5},
    {zp, isb, 5},
    {imp, inx, 2},
    {imm, sbc, 2},
    {imp, nop, 2},
    {imm, sbc, 2},
    {abso, cpx, 4},
    {abso, sbc, 4},
    {abso, inc, 6},
    {abso, isb, 6},

    /* F0 */
    {rel, beq, 2},
    {indy_p, sbc, 5},
    {imp, nop, 2},
    {indy, isb, 8},
    {zpx, nop, 4},
    {zpx, sbc, 4},
    {zpx, inc, 6},
    {zpx, isb, 6},
    {imp, sed, 2},
    {absy_p, sbc, 4},
    {imp, nop, 2},
    {absy, isb, 7},
    {absx_p, nop, 4},
    {absx_p, sbc, 4},
    {absx, inc, 7},
    {absx, isb, 7}
};

// -------------------------------------------------------------------
// Reset ve Interruptlar
// -------------------------------------------------------------------

void fake6502_reset(fake6502_context *c)
{
    int cold_boot =
        (c->emu.clockticks == 0 &&
         c->emu.instructions == 0);

    c->emu.pending_irq = 0;
    c->emu.branch_taken = 0;
    c->emu.branch_page_crossed = 0;
    c->emu.branch_irq_first_poll = 0;
    c->emu.branch_irq_second_poll = 0;

    if (cold_boot) {

        c->cpu.a = 0;
        c->cpu.x = 0;
        c->cpu.y = 0;

        c->cpu.s = 0x00;
        c->cpu.flags = 0x34;

        c->cpu.pc = 0x0000;

    } else {

        c->cpu.flags |=
            FAKE6502_INTERRUPT_FLAG;
    }

    (void)fake6502_mem_read(
        c,
        c->cpu.pc
    );

    (void)fake6502_mem_read(
        c,
        c->cpu.pc
    );

    for (int i = 0; i < 3; i++) {

        (void)fake6502_mem_read(
            c,
            FAKE6502_STACK_BASE + c->cpu.s
        );

        c->cpu.s--;
    }

    c->cpu.pc =
        fake6502_mem_read16(
            c,
            0xFFFC
        );

    c->emu.instructions = 0;
    c->emu.pending_irq = 0;
}

void fake6502_nmi(fake6502_context *c)
{
    g_nmi_ready = 0;
    c->emu.nmi_defer_after_interrupt = 0;

    fake6502_mem_read(
        c,
        c->cpu.pc
    );

    fake6502_mem_read(
        c,
        c->cpu.pc
    );

    fake6502_push_16(
        c,
        c->cpu.pc
    );

    uint8_t pushed_flags =
        (uint8_t)(
            (c->cpu.flags | 0x20) & ~0x10
        );

    fake6502_push_8(
        c,
        pushed_flags
    );

    fake6502_interrupt_set(c);

    c->cpu.pc =
        fake6502_mem_read16(
            c,
            0xFFFA
        );

    g_nmi_pending = 0;
    c->emu.nmi_defer_after_interrupt = 0;
}

void fake6502_irq(fake6502_context *c)
{
    int nmi_hijack = 0;

    fake6502_mem_read(
        c,
        c->cpu.pc
    );

    if (g_nmi_pending) {

        nmi_hijack = 1;
    }

    fake6502_mem_read(
        c,
        c->cpu.pc
    );

    if (g_nmi_pending &&
        !nmi_hijack) {

        nmi_hijack = 1;
    }

    fake6502_push_8(
        c,
        (c->cpu.pc >> 8) & 0xFF
    );

    if (g_nmi_pending &&
        !nmi_hijack) {

        nmi_hijack = 1;
    }

    fake6502_push_8(
        c,
        c->cpu.pc & 0xFF
    );

    if (g_nmi_pending &&
        !nmi_hijack) {

        nmi_hijack = 1;
    }

    fake6502_push_8(
        c,
        (c->cpu.flags | 0x20) & ~0x10
    );

    fake6502_interrupt_set(c);

    if (g_nmi_pending &&
        !nmi_hijack) {

        nmi_hijack = 1;
    }

    if (nmi_hijack) {

        c->cpu.pc =
            fake6502_mem_read16(
                c,
                0xFFFA
            );

        g_nmi_pending = 0;

        c->emu.nmi_defer_after_interrupt = 0;

    } else {

        c->cpu.pc =
            fake6502_mem_read16(
                c,
                0xFFFE
            );

        if (g_nmi_pending) {

            c->emu.nmi_defer_after_interrupt = 1;
        }
    }
}

void fake6502_step(fake6502_context *c)
{
    s_unstable_store_dma_halt = 0;

    uint8_t old_i =
        c->cpu.flags &
        FAKE6502_INTERRUPT_FLAG;

    c->emu.branch_taken = 0;
    c->emu.branch_page_crossed = 0;
    c->emu.branch_irq_first_poll = 0;
    c->emu.branch_irq_second_poll = 0;

    c->emu.instruction_pc =
        c->cpu.pc;

    uint8_t opcode =
        fake6502_mem_read(
            c,
            c->cpu.pc++
        );

    int irq_after_opcode_fetch =
        g_irq_pending;

    c->emu.branch_irq_first_poll =
        irq_after_opcode_fetch ? 1 : 0;

    c->emu.opcode =
        opcode;

    fake6502_opcodes[opcode].addr_mode(c);
    fake6502_opcodes[opcode].opcode(c);

    uint8_t new_i =
        c->cpu.flags &
        FAKE6502_INTERRUPT_FLAG;

    uint8_t poll_i =
        old_i;

    int irq_signal =
        g_irq_poll_snapshot;

    /*
     * NMI polling: fake6502_step, komut bittikten sonra g_nmi_pending
     * bayrağını g_nmi_ready'ye taşır. emu_driver_step_1 bu bayrağı bir
     * sonraki komut başında görüp NMI'yi servis eder. Bu blok, NMI
     * mekanizmasının tek tetikleyicisidir — kaldırılırsa NMI hiç ateşlenmez.
     */
    if (opcode != 0x00 && g_nmi_pending) {
        g_nmi_ready = 1;
    }

    if (opcode == 0x40) {

        poll_i =
            new_i;

        irq_signal =
            g_irq_pending;

    } else if (opcode == 0x00) {

        poll_i =
            new_i;

    } else if (
        fake6502_opcodes[opcode].addr_mode == rel
    ) {

        if (!c->emu.branch_taken) {

            irq_signal =
                c->emu.branch_irq_first_poll;

        } else if (
            c->emu.branch_page_crossed
        ) {

            irq_signal =
                c->emu.branch_irq_first_poll ||
                c->emu.branch_irq_second_poll;

        } else {

            irq_signal =
                c->emu.branch_irq_first_poll;
        }

    } else if (
        fake6502_opcodes[opcode].clockticks == 2
    ) {

        irq_signal =
            irq_after_opcode_fetch;
    }

    c->emu.pending_irq =
        (irq_signal && !poll_i)
            ? 1
            : 0;

    c->emu.instructions++;
}