/* UTILI: gen_fast.py tarafindan uretildi (fake6502.c komut tablosundan). ELLE DUZENLEMEYIN. */
/* Tam-komut hizli yolu (coklu komut, iki asamali dagitim): once adresleme modu (ea), sonra islem.
 * RAM / PRG-RAM / PRG erisimli resmi komutlarin, hicbir olay (deadline) beklemeyen halleri tek cagrida ardisik calistirilir;
 * yazmaclar ve bus alanlari yerel degiskenlerde kalir, durum cikista yazilir.
 * Dondurur: calisan cycle sayisi; 0 = ilk komut reddedildi (HICBIR durum degismedi: fake6502_step ile calistirin).
 * stop_tick != 0: clockticks >= stop_tick olunca durur (kare butcesi / gozlem siniri); 0: tek komut. */
enum { FM_IMP = 0, FM_ACC = 1, FM_IMM = 2, FM_ZP = 3, FM_ZPX = 4, FM_ZPY = 5, FM_ABSO = 6, FM_ABSX = 7, FM_ABSX_P = 8, FM_ABSY = 9, FM_ABSY_P = 10, FM_INDX = 11, FM_INDY = 12, FM_INDY_P = 13, FM_REL = 14, FM_NONE = 15 };
enum { FO_LDA = 0, FO_LDX = 1, FO_LDY = 2, FO_AND = 3, FO_ORA = 4, FO_EOR = 5, FO_ADC = 6, FO_SBC = 7, FO_CMP = 8, FO_CPX = 9, FO_CPY = 10, FO_BIT = 11, FO_ASL = 12, FO_LSR = 13, FO_ROL = 14, FO_ROR = 15, FO_INC = 16, FO_DEC = 17, FO_STA = 18, FO_STX = 19, FO_STY = 20, FO_ASL_A = 21, FO_LSR_A = 22, FO_ROL_A = 23, FO_ROR_A = 24, FO_INC_A = 25, FO_DEC_A = 26, FO_CLC = 27, FO_SEC = 28, FO_CLI = 29, FO_SEI = 30, FO_CLV = 31, FO_CLD = 32, FO_SED = 33, FO_TAX = 34, FO_TAY = 35, FO_TSX = 36, FO_TXA = 37, FO_TYA = 38, FO_TXS = 39, FO_INX = 40, FO_INY = 41, FO_DEX = 42, FO_DEY = 43, FO_NOP = 44, FO_BCC = 45, FO_BCS = 46, FO_BEQ = 47, FO_BNE = 48, FO_BMI = 49, FO_BPL = 50, FO_BVC = 51, FO_BVS = 52, FO_PHA = 53, FO_PHP = 54, FO_PLA = 55, FO_PLP = 56, FO_RTS = 57, FO_JSR = 58, FO_JMP = 59 };
#define FO_NOREAD 18   /* bu degerden kucuk islemler once RD(v, ea) yapar */
static const uint8_t FM_MODE_TAB[256] = {
    255, 11, 255, 255, 255, 3, 3, 255, 0, 2, 1, 255, 255, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
    15, 11, 255, 255, 3, 3, 3, 255, 0, 2, 1, 255, 6, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
    255, 11, 255, 255, 255, 3, 3, 255, 0, 2, 1, 255, 6, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
    0, 11, 255, 255, 255, 3, 3, 255, 0, 2, 1, 255, 255, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
    255, 11, 255, 255, 3, 3, 3, 255, 0, 255, 0, 255, 6, 6, 6, 255,
    14, 12, 255, 255, 4, 4, 5, 255, 0, 9, 0, 255, 255, 7, 255, 255,
    2, 11, 2, 255, 3, 3, 3, 255, 0, 2, 0, 255, 6, 6, 6, 255,
    14, 13, 255, 255, 4, 4, 5, 255, 0, 10, 0, 255, 8, 8, 10, 255,
    2, 11, 255, 255, 3, 3, 3, 255, 0, 2, 0, 255, 6, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
    2, 11, 255, 255, 3, 3, 3, 255, 0, 2, 0, 2, 6, 6, 6, 255,
    14, 13, 255, 255, 255, 4, 4, 255, 0, 10, 255, 255, 255, 8, 7, 255,
};
static const uint8_t FM_OP_TAB[256] = {
    255, 4, 255, 255, 255, 4, 12, 255, 54, 4, 21, 255, 255, 4, 12, 255,
    50, 4, 255, 255, 255, 4, 12, 255, 27, 4, 255, 255, 255, 4, 12, 255,
    58, 3, 255, 255, 11, 3, 14, 255, 56, 3, 23, 255, 11, 3, 14, 255,
    49, 3, 255, 255, 255, 3, 14, 255, 28, 3, 255, 255, 255, 3, 14, 255,
    255, 5, 255, 255, 255, 5, 13, 255, 53, 5, 22, 255, 59, 5, 13, 255,
    51, 5, 255, 255, 255, 5, 13, 255, 29, 5, 255, 255, 255, 5, 13, 255,
    57, 6, 255, 255, 255, 6, 15, 255, 55, 6, 24, 255, 255, 6, 15, 255,
    52, 6, 255, 255, 255, 6, 15, 255, 30, 6, 255, 255, 255, 6, 15, 255,
    255, 18, 255, 255, 20, 18, 19, 255, 43, 255, 37, 255, 20, 18, 19, 255,
    45, 18, 255, 255, 20, 18, 19, 255, 38, 18, 39, 255, 255, 18, 255, 255,
    2, 0, 1, 255, 2, 0, 1, 255, 35, 0, 34, 255, 2, 0, 1, 255,
    46, 0, 255, 255, 2, 0, 1, 255, 31, 0, 36, 255, 2, 0, 1, 255,
    10, 8, 255, 255, 10, 8, 17, 255, 41, 8, 42, 255, 10, 8, 17, 255,
    48, 8, 255, 255, 255, 8, 17, 255, 32, 8, 255, 255, 255, 8, 17, 255,
    9, 7, 255, 255, 9, 7, 16, 255, 40, 7, 44, 7, 9, 7, 16, 255,
    47, 7, 255, 255, 255, 7, 16, 255, 33, 7, 255, 255, 255, 7, 16, 255,
};
int fake6502_run_fast(fake6502_context *c, int stop_tick, int *n_out)
{
    nes_bus_t *const bus = (nes_bus_t *)c->state_host;
    if (__builtin_expect(bus == 0 || bus->apu.dmc.dma_request != 0, 0)) return 0;

    uint8_t *const mem = bus->mem;
    const int clk0 = c->emu.clockticks;
    int clk = clk0;
    const int multi = (stop_tick != 0 && !g_nmi_ready && !c->emu.nmi_defer_after_interrupt && !bus->oam_dma_pending);
    uint16_t pc = c->cpu.pc;
    uint8_t a = c->cpu.a, x = c->cpu.x, y = c->cpu.y, s = c->cpu.s, fl = c->cpu.flags;
    uint8_t lv = bus->open_bus_value;
    uint16_t lra = bus->last_cpu_read_addr;
    uint16_t ipc = pc;
    uint8_t opcode = 0, pend = 0, old_i = 0;
    int n = 0, wrote = 0, touched = 0;

    for (;;) {
        if (__builtin_expect((int32_t)((uint32_t)(clk + 8) - (uint32_t)g_dl_min) >= 0, 0)) goto done;
        {
        const uint16_t pc0 = pc; const uint8_t a0 = a, x0 = x, y0 = y, s0 = s, fl0 = fl, lv0 = lv; const uint16_t lra0 = lra;
        uint16_t ea = 0, base;
        uint8_t v = 0, t_ = 0, h_ = 0, l_ = 0, d_;
        int ticks = 0, taken = 0;
        (void)d_; (void)base; (void)ea; (void)v; (void)t_; (void)h_; (void)l_; (void)taken;
        ipc = pc; old_i = (uint8_t)(fl & FAKE6502_INTERRUPT_FLAG);
        RD(opcode, pc); pc++;
        const unsigned md_ = FM_MODE_TAB[opcode];
        if (__builtin_expect(md_ == 255u, 0)) goto abort_i;
        const unsigned op_ = FM_OP_TAB[opcode];
        switch (md_) {
        case FM_IMP: RD(d_, pc); break;
        case FM_ACC: RD(d_, pc); break;
        case FM_IMM: ea = pc; pc++; break;
        case FM_ZP: RD(t_, pc); pc++; ea = t_; break;
        case FM_ZPX: RD(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); break;
        case FM_ZPY: RD(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + y); break;
        case FM_ABSO: RD(t_, pc); RD(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); break;
        case FM_ABSX: RD(t_, pc); RD(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); break;
        case FM_ABSX_P: RD(t_, pc); RD(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_ABSY: RD(t_, pc); RD(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y); break;
        case FM_ABSY_P: RD(t_, pc); RD(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_INDX: RD(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); break;
        case FM_INDY: RD(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y); break;
        case FM_INDY_P: RD(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_REL: RD(t_, pc); pc++; break;
        case FM_NONE:  break;
        default: goto abort_i;
        }
        if (op_ < FO_NOREAD) RD(v, ea);
        switch (op_) {
        case FO_LDA: a = v; NZ(a); break;
        case FO_LDX: x = v; NZ(x); break;
        case FO_LDY: y = v; NZ(y); break;
        case FO_AND: a = a & v; NZ(a); break;
        case FO_ORA: a = a | v; NZ(a); break;
        case FO_EOR: a = a ^ v; NZ(a); break;
        case FO_ADC: ADD8(v); break;
        case FO_SBC: ADD8((uint8_t)(v ^ 0xFF)); break;
        case FO_CMP: CMPV(a, v); break;
        case FO_CPX: CMPV(x, v); break;
        case FO_CPY: CMPV(y, v); break;
        case FO_BIT: fl = (uint8_t)((((fl & ~FAKE6502_ZERO_FLAG) | ((a & v) ? 0 : FAKE6502_ZERO_FLAG)) & 0x3F) | (v & 0xC0)); break;
        case FO_ASL: { uint8_t r; uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_LSR: { uint8_t r; r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break;
        case FO_ROL: { uint8_t r; uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_ROR: { uint8_t r; r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_INC: { uint8_t r; r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break;
        case FO_DEC: { uint8_t r; r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break;
        case FO_STA: WR(ea, a); break;
        case FO_STX: WR(ea, x); break;
        case FO_STY: WR(ea, y); break;
        case FO_ASL_A: { uint8_t r; v = a; uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); a = r; } break;
        case FO_LSR_A: { uint8_t r; v = a; r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); a = r; } break;
        case FO_ROL_A: { uint8_t r; v = a; uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); a = r; } break;
        case FO_ROR_A: { uint8_t r; v = a; r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); a = r; } break;
        case FO_INC_A: { uint8_t r; v = a; r = (uint8_t)(v + 1); NZ(r); a = r; } break;
        case FO_DEC_A: { uint8_t r; v = a; r = (uint8_t)(v - 1); NZ(r); a = r; } break;
        case FO_CLC: fl &= ~FAKE6502_CARRY_FLAG; break;
        case FO_SEC: fl |= FAKE6502_CARRY_FLAG; break;
        case FO_CLI: fl &= ~FAKE6502_INTERRUPT_FLAG; break;
        case FO_SEI: fl |= FAKE6502_INTERRUPT_FLAG; break;
        case FO_CLV: fl &= ~FAKE6502_OVERFLOW_FLAG; break;
        case FO_CLD: fl &= ~FAKE6502_DECIMAL_FLAG; break;
        case FO_SED: fl |= FAKE6502_DECIMAL_FLAG; break;
        case FO_TAX: x = a; NZ(x); break;
        case FO_TAY: y = a; NZ(y); break;
        case FO_TSX: x = s; NZ(x); break;
        case FO_TXA: a = x; NZ(a); break;
        case FO_TYA: a = y; NZ(a); break;
        case FO_TXS: s = x; break;
        case FO_INX: x = (uint8_t)(x + 1); NZ(x); break;
        case FO_INY: y = (uint8_t)(y + 1); NZ(y); break;
        case FO_DEX: x = (uint8_t)(x - 1); NZ(x); break;
        case FO_DEY: y = (uint8_t)(y - 1); NZ(y); break;
        case FO_NOP:  break;
        case FO_BCC: taken = (!(fl & FAKE6502_CARRY_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BCS: taken = ((fl & FAKE6502_CARRY_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BEQ: taken = ((fl & FAKE6502_ZERO_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BNE: taken = (!(fl & FAKE6502_ZERO_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BMI: taken = ((fl & FAKE6502_SIGN_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BPL: taken = (!(fl & FAKE6502_SIGN_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BVC: taken = (!(fl & FAKE6502_OVERFLOW_FLAG)) ? 1 : 0; goto branch_common;
        case FO_BVS: taken = ((fl & FAKE6502_OVERFLOW_FLAG)) ? 1 : 0; goto branch_common;
        case FO_PHA: WR((uint16_t)(0x100 + s), a); s--; break;
        case FO_PHP: WR((uint16_t)(0x100 + s), (uint8_t)(fl | 0x30)); s--; break;
        case FO_PLA: ticks++; s++; RD(v, (uint16_t)(0x100 + s)); a = v; NZ(a); break;
        case FO_PLP: ticks++; s++; RD(v, (uint16_t)(0x100 + s)); fl = (uint8_t)((v & ~0x10) | 0x20); break;
        case FO_RTS: ticks++; s++; RD(t_, (uint16_t)(0x100 + s)); s++; RD(h_, (uint16_t)(0x100 + s)); pc = (uint16_t)(t_ | (h_ << 8)); RD(d_, pc); pc++; break;
        case FO_JSR: RD(t_, pc); pc++; CHKR(pc); RD(d_, (uint16_t)(0x100 + s)); WR((uint16_t)(0x100 + s), (uint8_t)(pc >> 8)); s--; WR((uint16_t)(0x100 + s), (uint8_t)(pc & 0xFF)); s--; RD(h_, pc); pc = (uint16_t)(t_ | (h_ << 8)); break;
        case FO_JMP: pc = ea; break;
        default: goto abort_i;
        }
        goto instr_done;
        branch_common:
        if (taken) {
            const uint16_t opc_ = pc;
            ea = (uint16_t)(pc + (int8_t)t_); RD(d_, opc_); pc = ea;
            if ((opc_ & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((opc_ & 0xFF00) | (ea & 0x00FF)));
        }
        instr_done:
        clk += ticks; n++;
        pend = (uint8_t)((g_irq_pending && !old_i) ? 1 : 0);
        if (g_nmi_pending) goto done;
        if (pend) goto done;
        if (g_ff_stop != 0 && pc <= ipc) goto done;
        if (!multi || clk >= stop_tick) goto done;
        continue;

        abort_i:
        pc = pc0; a = a0; x = x0; y = y0; s = s0; fl = fl0; lv = lv0; lra = lra0;
        goto done;
        }
    }

done:
    if (n == 0) return 0;
    s_unstable_store_dma_halt = 0;
    c->emu.instruction_pc = ipc;
    c->emu.opcode = opcode;
    c->cpu.pc = pc; c->cpu.a = a; c->cpu.x = x; c->cpu.y = y; c->cpu.s = s; c->cpu.flags = fl;
    c->emu.clockticks = clk;
    bus->last_cpu_read_addr = lra;
    bus->open_bus_value = lv;
    bus->cpu_internal_bus_value = lv;
    if (bus->in) bus->in->pad1_last_bus_was_4016 = 0;
    g_irq_poll_snapshot = g_irq_pending;
    if (wrote | touched) g_write_epoch++;
    if (g_nmi_pending) g_nmi_ready = 1;
    c->emu.pending_irq = pend;
    c->emu.instructions += n;
    *n_out = n;
    return clk - clk0;
}
