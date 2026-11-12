/* UTILI: gen_fast.py tarafindan uretildi (fake6502.c komut tablosundan). ELLE DUZENLEMEYIN. */
/* Tam-komut hizli yolu (coklu komut, iki asamali dagitim): once adresleme modu (ea), sonra islem.
 * RAM / PRG-RAM / PRG erisimli resmi komutlarin, hicbir olay (deadline) beklemeyen halleri tek cagrida ardisik calistirilir;
 * yazmaclar ve bus alanlari yerel degiskenlerde kalir, durum cikista yazilir.
 * Dondurur: calisan cycle sayisi; 0 = ilk komut reddedildi (HICBIR durum degismedi: fake6502_step ile calistirin).
 * stop_tick != 0: clockticks >= stop_tick olunca durur (kare butcesi / gozlem siniri); 0: tek komut. */
enum { FM_IMP = 0, FM_ACC = 1, FM_IMM = 2, FM_ZP = 3, FM_ZPX = 4, FM_ZPY = 5, FM_ABSO = 6, FM_ABSX = 7, FM_ABSX_P = 8, FM_ABSY = 9, FM_ABSY_P = 10, FM_INDX = 11, FM_INDY = 12, FM_INDY_P = 13, FM_REL = 14, FM_NONE = 15 };
enum { FO_LDA = 0, FO_LDX = 1, FO_LDY = 2, FO_AND = 3, FO_ORA = 4, FO_EOR = 5, FO_ADC = 6, FO_SBC = 7, FO_CMP = 8, FO_CPX = 9, FO_CPY = 10, FO_BIT = 11, FO_ASL = 12, FO_LSR = 13, FO_ROL = 14, FO_ROR = 15, FO_INC = 16, FO_DEC = 17, FO_STA = 18, FO_STX = 19, FO_STY = 20, FO_ASL_A = 21, FO_LSR_A = 22, FO_ROL_A = 23, FO_ROR_A = 24, FO_INC_A = 25, FO_DEC_A = 26, FO_CLC = 27, FO_SEC = 28, FO_CLI = 29, FO_SEI = 30, FO_CLV = 31, FO_CLD = 32, FO_SED = 33, FO_TAX = 34, FO_TAY = 35, FO_TSX = 36, FO_TXA = 37, FO_TYA = 38, FO_TXS = 39, FO_INX = 40, FO_INY = 41, FO_DEX = 42, FO_DEY = 43, FO_NOP = 44, FO_BCC = 45, FO_BCS = 46, FO_BEQ = 47, FO_BNE = 48, FO_BMI = 49, FO_BPL = 50, FO_BVC = 51, FO_BVS = 52, FO_PHA = 53, FO_PHP = 54, FO_PLA = 55, FO_PLP = 56, FO_RTS = 57, FO_JSR = 58, FO_JMP = 59 };
/* EXP_FAST_DISPATCH 1: opcode basina tek case (asagida); 0: eski iki asamali dagitim (mod, sonra islem). Davranis ayni. */
#ifndef EXP_FAST_DISPATCH
#define EXP_FAST_DISPATCH 1
#endif
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
    /* PRG goruntusu: cagri boyunca degismez (mapper yazimi reddedilir; tek yavas komut cikista calisir). Yerel kopya. */
#if EXP_PRG_PTR
    const uint8_t *const pimg = nes_prg_img;
#endif
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
    int slow_ = 0;   /* 1: reddedilen komut yavas cekirdekle bu cagrida calistirilacak (en az bir hizli komuttan sonra) */
    int pr_ = 0;     /* son geri dallanmadan beri hizli yolda $2002 okundu */
    /* geri dallanma ozeti (bos dongu adayi suzgeci); wn: bu cagrida yazma + PRG-RAM okuma sayisi */
    unsigned wn = 0, bb_wn = 0;
    uint16_t bb_pc = 0; uint8_t bb_a = 0, bb_x = 0, bb_y = 0, bb_s = 0, bb_fl = 0; int bb_valid = 0;
    const int after_slow = g_rf_after_slow; g_rf_after_slow = 0;
    /* Onceki cagri bir yoklama turunun sonunda (ayni durumlu geri dallanma) dondu: giris durumu o gozlemin durumudur; turun bir
     * sonraki tekrari ilk geri dallanmada yakalanir (surucu yoklama atlamasi ardisik turlarda gozlem ister). */
    if (g_rf_poll_ret) { g_rf_poll_ret = 0; bb_valid = 1; bb_pc = pc; bb_a = a; bb_x = x; bb_y = y; bb_s = s; bb_fl = fl; bb_wn = 0; }
    /* Dongu boyunca DEGISMEYEN globaller (dongude PPU/APU senkronu yok; cagrilan dis islevler: saf mapper_prg_ram_* ve emu_driver_fast_status_read (bu globallere dokunmaz)):
     * yerel kopyalar. Bayt yazimlari her seyle takma ad olusturabildiginden derleyici aksi halde her yazimdan sonra yeniden yukler. */
    const int dl_ = g_dl_min, irq_ = g_irq_pending, nmi_ = g_nmi_pending, ffs_ = g_ff_stop;
    /* komut sonu durdurma siniri: tek-komut kipi veya bekleyen NMI -> ilk komuttan sonra (her komut >= 2 cycle) */
    const int lim_ = (multi && !nmi_) ? stop_tick : clk0 + 1;

    for (;;) {
        if (__builtin_expect((int32_t)((uint32_t)(clk + 8) - (uint32_t)dl_) >= 0, 0)) goto done;
        {
        /* Reddetme (abort_i) yalniz pc, s ve bus alanlarini geri yukler: a/x/y/fl hicbir reddetme noktasindan once degismez
         * (bellek RMW komutlari yazilabilirligi CHKW ile once denetler; RTS hedefi G/C alaninda olabilir -> s geri yuklenir). */
        const uint8_t s0 = s, lv0 = lv; const uint16_t lra0 = lra;
        uint16_t ea = 0, base;
        uint8_t v = 0, t_ = 0, h_ = 0, l_ = 0, d_;
        int ticks = 0, taken = 0;
        (void)d_; (void)base; (void)ea; (void)v; (void)t_; (void)h_; (void)l_; (void)taken;
        ipc = pc; old_i = (uint8_t)(fl & FAKE6502_INTERRUPT_FLAG);
#if EXP_BENCH && EXP_BENCH_PC
        { extern uint32_t g_pch[65536]; extern int g_pch_on; if (g_pch_on) g_pch[pc]++; }
#endif
        RDP(opcode, pc); pc++;
#if EXP_FAST_DISPATCH
        /* Tek asamali dagitim: her opcode icin adresleme + okuma + islem tek case (gen_disp.py ile, iki asamali surumden
         * mekanik olarak uretildi; davranis birebir ayni). */
        switch (opcode) {
        case 0x01: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x05: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x06: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x08: { RDP(d_, pc);  WR((uint16_t)(0x100 + s), (uint8_t)(fl | 0x30)); s--; break; }
        case 0x09: { ea = pc; pc++; RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x0A: { RDP(d_, pc);  { uint8_t r; v = a; uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); a = r; } break; }
        case 0x0D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x0E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x10: { RDP(t_, pc); pc++;  taken = (!(fl & FAKE6502_SIGN_FLAG)) ? 1 : 0; goto branch_common; }
        case 0x11: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x15: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x16: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x18: { RDP(d_, pc);  fl &= ~FAKE6502_CARRY_FLAG; break; }
        case 0x19: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x1D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a | v; NZ(a); break; }
        case 0x1E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x20: {   RDP(t_, pc); pc++; CHKR(pc); RD(d_, (uint16_t)(0x100 + s)); WR((uint16_t)(0x100 + s), (uint8_t)(pc >> 8)); s--; WR((uint16_t)(0x100 + s), (uint8_t)(pc & 0xFF)); s--; RD(h_, pc); pc = (uint16_t)(t_ | (h_ << 8)); break; }
        case 0x21: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x24: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); fl = (uint8_t)((((fl & ~FAKE6502_ZERO_FLAG) | ((a & v) ? 0 : FAKE6502_ZERO_FLAG)) & 0x3F) | (v & 0xC0)); break; }
        case 0x25: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x26: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x28: { RDP(d_, pc);  ticks++; s++; RD(v, (uint16_t)(0x100 + s)); fl = (uint8_t)((v & ~0x10) | 0x20); break; }
        case 0x29: { ea = pc; pc++; RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x2A: { RDP(d_, pc);  { uint8_t r; v = a; uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); a = r; } break; }
        case 0x2C: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); fl = (uint8_t)((((fl & ~FAKE6502_ZERO_FLAG) | ((a & v) ? 0 : FAKE6502_ZERO_FLAG)) & 0x3F) | (v & 0xC0)); break; }
        case 0x2D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x2E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x30: { RDP(t_, pc); pc++;  taken = ((fl & FAKE6502_SIGN_FLAG)) ? 1 : 0; goto branch_common; }
        case 0x31: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x35: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x36: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x38: { RDP(d_, pc);  fl |= FAKE6502_CARRY_FLAG; break; }
        case 0x39: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x3D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a & v; NZ(a); break; }
        case 0x3E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x41: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x45: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x46: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break; }
        case 0x48: { RDP(d_, pc);  WR((uint16_t)(0x100 + s), a); s--; break; }
        case 0x49: { ea = pc; pc++; RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x4A: { RDP(d_, pc);  { uint8_t r; v = a; r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); a = r; } break; }
        case 0x4C: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8));  pc = ea;
#if EXP_INCLOOP_FF
            /* Sayac bekleme dongusu: [ea, ipc) yalniz "INC zp" / "INC abs (dahili RAM)" komutlarindan olusuyor ve ipc'deki
             * JMP ea'ya donuyorsa (PRG'de), bir tur = her INC hedefini 1 artirir, baska hicbir sey okumaz/yazmaz/dallanmaz.
             * Tur sonunda (simdi) durum: pc = ea, N/Z = son INC sonucunun NZ'si (JMP bayrak degistirmez). k tur sonrasi kesin:
             * her hedef += (turdaki INC sayisi) * k, N/Z = son hedefin yeni degerinin NZ'si, cycle += k * (5|6 + ... + 3),
             * komut += k * (INC sayisi + 1). Bus alanlari (lra/lv = JMP'nin ust bayt okumasi) her turda ayni. Normal
             * yurutmenin de durmayacagi kadar tur atlanir (sonraki komut oncesi clk + 8 < dl_, komut sonu clk < lim_);
             * bekleyen IRQ I=0 iken atlanmaz
             * (I=1 iken dongu boyunca maskeli kalir: dongu I bayragini degistirmez). */
            if ((!irq_ || (fl & FAKE6502_INTERRUPT_FLAG)) && ea >= 0x8000u && ea < ipc && (unsigned)(ipc - ea) <= 24u) {
                uint16_t q = ea; int cyc = 3, ni = 0, okl = 1;
                uint16_t ad[8]; uint8_t lastk = 0;
                while (q < ipc && okl) {
                    const uint8_t o = FAST_PRG(q);
                    if (o == 0xE6u && ni < 8) { ad[ni++] = FAST_PRG((uint16_t)(q + 1)); cyc += 5; q = (uint16_t)(q + 2); }
                    else if (o == 0xEEu && ni < 8) {
                        const uint16_t aa = (uint16_t)(FAST_PRG((uint16_t)(q + 1)) | (FAST_PRG((uint16_t)(q + 2)) << 8));
                        if (aa >= 0x2000u) okl = 0;
                        else { ad[ni++] = (uint16_t)(aa & 0x7FFu); cyc += 6; q = (uint16_t)(q + 3); }
                    } else okl = 0;
                }
                if (okl && q == ipc && ni > 0) {
                    const int now_ = clk + ticks;
                    int k = 1 << 20;
                    if ((int32_t)((uint32_t)dl_ - (uint32_t)now_) < 9 + cyc || (int32_t)((uint32_t)lim_ - (uint32_t)now_) < 1 + cyc) k = 0;
                    else {
                        const int k_dl = (int)((uint32_t)dl_ - (uint32_t)now_ - 9u) / cyc;
                        const int k_lim = (int)((uint32_t)lim_ - (uint32_t)now_ - 1u) / cyc;
                        if (k_dl < k) k = k_dl;
                        if (k_lim < k) k = k_lim;
                    }
                    if (k > 0) {
                        /* zp adresleri de ad[] icinde (0..255); her hedefi tur basina artis sayisi kadar ilerlet */
                        for (int i = 0; i < ni; i++) mem[ad[i] & 0x7FFu] = (uint8_t)(mem[ad[i] & 0x7FFu] + (uint8_t)k);
                        lastk = mem[ad[ni - 1] & 0x7FFu];
                        NZ(lastk);
                        ticks += cyc * k; n += (ni + 1) * k;
                        g_ff_cycles_total += (unsigned long)(cyc * k);
                    }
                }
            }
#endif
            break; }
        case 0x4D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x4E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break; }
        case 0x50: { RDP(t_, pc); pc++;  taken = (!(fl & FAKE6502_OVERFLOW_FLAG)) ? 1 : 0; goto branch_common; }
        case 0x51: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x55: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x56: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break; }
        case 0x58: { RDP(d_, pc);  fl &= ~FAKE6502_INTERRUPT_FLAG; break; }
        case 0x59: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x5D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = a ^ v; NZ(a); break; }
        case 0x5E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break; }
        case 0x60: { RDP(d_, pc);  ticks++; s++; RD(t_, (uint16_t)(0x100 + s)); s++; RD(h_, (uint16_t)(0x100 + s)); pc = (uint16_t)(t_ | (h_ << 8)); RD(d_, pc); pc++; break; }
        case 0x61: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); ADD8(v); break; }
        case 0x65: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); ADD8(v); break; }
        case 0x66: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x68: { RDP(d_, pc);  ticks++; s++; RD(v, (uint16_t)(0x100 + s)); a = v; NZ(a); break; }
        case 0x69: { ea = pc; pc++; RDV(v, ea); ADD8(v); break; }
        case 0x6A: { RDP(d_, pc);  { uint8_t r; v = a; r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); a = r; } break; }
        case 0x6D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); ADD8(v); break; }
        case 0x6E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x70: { RDP(t_, pc); pc++;  taken = ((fl & FAKE6502_OVERFLOW_FLAG)) ? 1 : 0; goto branch_common; }
        case 0x71: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8(v); break; }
        case 0x75: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); ADD8(v); break; }
        case 0x76: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x78: { RDP(d_, pc);  fl |= FAKE6502_INTERRUPT_FLAG; break; }
        case 0x79: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8(v); break; }
        case 0x7D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8(v); break; }
        case 0x7E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break; }
        case 0x81: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8));  WR(ea, a); break; }
        case 0x84: { RDP(t_, pc); pc++; ea = t_;  WR(ea, y); break; }
        case 0x85: { RDP(t_, pc); pc++; ea = t_;  WR(ea, a); break; }
        case 0x86: { RDP(t_, pc); pc++; ea = t_;  WR(ea, x); break; }
        case 0x88: { RDP(d_, pc);  y = (uint8_t)(y - 1); NZ(y); break; }
        case 0x8A: { RDP(d_, pc);  a = x; NZ(a); break; }
        case 0x8C: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8));  WR(ea, y); break; }
        case 0x8D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8));  WR(ea, a); break; }
        case 0x8E: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8));  WR(ea, x); break; }
        case 0x90: { RDP(t_, pc); pc++;  taken = (!(fl & FAKE6502_CARRY_FLAG)) ? 1 : 0; goto branch_common; }
        case 0x91: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y);  WR(ea, a); break; }
        case 0x94: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x);  WR(ea, y); break; }
        case 0x95: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x);  WR(ea, a); break; }
        case 0x96: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + y);  WR(ea, x); break; }
        case 0x98: { RDP(d_, pc);  a = y; NZ(a); break; }
        case 0x99: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y);  WR(ea, a); break; }
        case 0x9A: { RDP(d_, pc);  s = x; break; }
        case 0x9D: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x);  WR(ea, a); break; }
        case 0xA0: { ea = pc; pc++; RDV(v, ea); y = v; NZ(y); break; }
        case 0xA1: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); a = v; NZ(a); break; }
        case 0xA2: { ea = pc; pc++; RDV(v, ea); x = v; NZ(x); break; }
        case 0xA4: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); y = v; NZ(y); break; }
        case 0xA5: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); a = v; NZ(a); break; }
        case 0xA6: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); x = v; NZ(x); break; }
        case 0xA8: { RDP(d_, pc);  y = a; NZ(y); break; }
        case 0xA9: { ea = pc; pc++; RDV(v, ea); a = v; NZ(a); break; }
        case 0xAA: { RDP(d_, pc);  x = a; NZ(x); break; }
        case 0xAC: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); y = v; NZ(y); break; }
        case 0xAD: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); a = v; NZ(a); break; }
        case 0xAE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); x = v; NZ(x); break; }
        case 0xB0: { RDP(t_, pc); pc++;  taken = ((fl & FAKE6502_CARRY_FLAG)) ? 1 : 0; goto branch_common; }
        case 0xB1: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = v; NZ(a); break; }
        case 0xB4: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); y = v; NZ(y); break; }
        case 0xB5: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); a = v; NZ(a); break; }
        case 0xB6: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + y); RDV(v, ea); x = v; NZ(x); break; }
        case 0xB8: { RDP(d_, pc);  fl &= ~FAKE6502_OVERFLOW_FLAG; break; }
        case 0xB9: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = v; NZ(a); break; }
        case 0xBA: { RDP(d_, pc);  x = s; NZ(x); break; }
        case 0xBC: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); y = v; NZ(y); break; }
        case 0xBD: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); a = v; NZ(a); break; }
        case 0xBE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); x = v; NZ(x); break; }
        case 0xC0: { ea = pc; pc++; RDV(v, ea); CMPV(y, v); break; }
        case 0xC1: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); CMPV(a, v); break; }
        case 0xC4: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); CMPV(y, v); break; }
        case 0xC5: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); CMPV(a, v); break; }
        case 0xC6: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xC8: { RDP(d_, pc);  y = (uint8_t)(y + 1); NZ(y); break; }
        case 0xC9: { ea = pc; pc++; RDV(v, ea); CMPV(a, v); break; }
        case 0xCA: { RDP(d_, pc);  x = (uint8_t)(x - 1); NZ(x); break; }
        case 0xCC: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); CMPV(y, v); break; }
        case 0xCD: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); CMPV(a, v); break; }
        case 0xCE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xD0: { RDP(t_, pc); pc++;  taken = (!(fl & FAKE6502_ZERO_FLAG)) ? 1 : 0; goto branch_common; }
        case 0xD1: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); CMPV(a, v); break; }
        case 0xD5: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); CMPV(a, v); break; }
        case 0xD6: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xD8: { RDP(d_, pc);  fl &= ~FAKE6502_DECIMAL_FLAG; break; }
        case 0xD9: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); CMPV(a, v); break; }
        case 0xDD: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); CMPV(a, v); break; }
        case 0xDE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xE0: { ea = pc; pc++; RDV(v, ea); CMPV(x, v); break; }
        case 0xE1: { RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xE4: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); CMPV(x, v); break; }
        case 0xE5: { RDP(t_, pc); pc++; ea = t_; RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xE6: { RDP(t_, pc); pc++; ea = t_; RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xE8: { RDP(d_, pc);  x = (uint8_t)(x + 1); NZ(x); break; }
        case 0xE9: { ea = pc; pc++; RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xEA: { RDP(d_, pc);  break; }
        case 0xEB: { ea = pc; pc++; RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xEC: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); CMPV(x, v); break; }
        case 0xED: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xEE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xF0: { RDP(t_, pc); pc++;  taken = ((fl & FAKE6502_ZERO_FLAG)) ? 1 : 0; goto branch_common; }
        case 0xF1: { RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xF5: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xF6: { RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        case 0xF8: { RDP(d_, pc);  fl |= FAKE6502_DECIMAL_FLAG; break; }
        case 0xF9: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xFD: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); RDV(v, ea); ADD8((uint8_t)(v ^ 0xFF)); break; }
        case 0xFE: { RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); RD(v, ea); { uint8_t r; CHKW(ea); r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break; }
        default: goto abort_i;
        }
#else
        const unsigned md_ = FM_MODE_TAB[opcode];
        if (__builtin_expect(md_ == 255u, 0)) goto abort_i;
        const unsigned op_ = FM_OP_TAB[opcode];
        switch (md_) {
        case FM_IMP: RDP(d_, pc); break;
        case FM_ACC: RDP(d_, pc); break;
        case FM_IMM: ea = pc; pc++; break;
        case FM_ZP: RDP(t_, pc); pc++; ea = t_; break;
        case FM_ZPX: RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + x); break;
        case FM_ZPY: RDP(t_, pc); pc++; RD(d_, t_); ea = (uint8_t)(t_ + y); break;
        case FM_ABSO: RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; ea = (uint16_t)(t_ | (h_ << 8)); break;
        case FM_ABSX: RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + x) & 0x00FF))); ea = (uint16_t)(base + x); break;
        case FM_ABSX_P: RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + x); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_ABSY: RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y); break;
        case FM_ABSY_P: RDP(t_, pc); RDP(h_, (uint16_t)(pc + 1)); pc += 2; base = (uint16_t)(t_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_INDX: RDP(t_, pc); pc++; RD(d_, t_); t_ = (uint8_t)(t_ + x); RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); ea = (uint16_t)(l_ | (h_ << 8)); break;
        case FM_INDY: RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); RD(d_, (uint16_t)((base & 0xFF00) | ((base + y) & 0x00FF))); ea = (uint16_t)(base + y); break;
        case FM_INDY_P: RDP(t_, pc); pc++; RD(l_, t_); RD(h_, (uint8_t)(t_ + 1)); base = (uint16_t)(l_ | (h_ << 8)); ea = (uint16_t)(base + y); if ((base & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((base & 0xFF00) | (ea & 0x00FF))); break;
        case FM_REL: RDP(t_, pc); pc++; break;
        case FM_NONE:  break;
        default: goto abort_i;
        }
        /* RDV ($2002 hizli okumasi) yalniz salt okuma islemlerinde: ardindan bus erisimi yok, komut reddedilemez. RMW (ASL..DEC)
         * okumadan sonra yazar ve $2002'ye yazma reddedilir: okuma yan etkileri iki kez sayilirdi. */
        if (op_ <= FO_BIT) RDV(v, ea); else if (op_ < FO_NOREAD) RD(v, ea);
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
        case FO_ASL: { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)(v << 1); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_LSR: { uint8_t r; CHKW(ea); r = (uint8_t)(v >> 1); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2)); WR(ea, v); WR(ea, r); } break;
        case FO_ROL: { uint8_t r; CHKW(ea); uint16_t tt_ = (uint16_t)((v << 1) | (fl & 1)); r = (uint8_t)tt_; fl = (uint8_t)((fl & ~0x83) | ((tt_ & 0x100) ? 1 : 0) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_ROR: { uint8_t r; CHKW(ea); r = (uint8_t)((v >> 1) | ((fl & 1) << 7)); fl = (uint8_t)((fl & ~0x83) | (v & 1) | (r ? 0 : 2) | (r & 0x80)); WR(ea, v); WR(ea, r); } break;
        case FO_INC: { uint8_t r; CHKW(ea); r = (uint8_t)(v + 1); NZ(r); WR(ea, v); WR(ea, r); } break;
        case FO_DEC: { uint8_t r; CHKW(ea); r = (uint8_t)(v - 1); NZ(r); WR(ea, v); WR(ea, r); } break;
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
        case FO_JSR: RDP(t_, pc); pc++; CHKR(pc); RD(d_, (uint16_t)(0x100 + s)); WR((uint16_t)(0x100 + s), (uint8_t)(pc >> 8)); s--; WR((uint16_t)(0x100 + s), (uint8_t)(pc & 0xFF)); s--; RD(h_, pc); pc = (uint16_t)(t_ | (h_ << 8)); break;
        case FO_JMP: pc = ea; break;
        default: goto abort_i;
        }
#endif
        goto instr_done;
        branch_common:
        if (taken) {
            const uint16_t opc_ = pc;
            ea = (uint16_t)(pc + (int8_t)t_); RD(d_, opc_); pc = ea;
            if ((opc_ & 0xFF00) != (ea & 0xFF00)) RD(d_, (uint16_t)((opc_ & 0xFF00) | (ea & 0x00FF)));
#if EXP_DELAY_FF
            /* Gecikme dongusu "DEY|DEX ; BNE *-1" (D0 FD), ayni sayfada, PRG'de: BNE alindiktan sonra (pc = DEY/DEX basi)
             * durum r (Y|X) ve N/Z = NZ(r) ise sonraki turlar tamamen belirlidir: tur = DEC (2 cycle: opcode + bos okuma) +
             * BNE alindi (3 cycle), r != 0 kaldikca. r-1 tura kadar, normal yurutmenin de durmayacagi kadar tur (sonraki
             * komut oncesi clk + 8 < dl_, komut sonu clk < lim_) kapali formda atlanir. Bus alanlari (lra/lv: BNE'nin bos
             * okumasi) her turda ayni; komut sayaci 2/tur. Bekleyen IRQ I=0 iken atlanmaz (tur arasinda servis
             * edilebilir); I=1 iken dongu boyunca maskeli kalir. */
            if (opcode == 0xD0 && t_ == 0xFDu && (!irq_ || (fl & FAKE6502_INTERRUPT_FLAG)) && ea >= 0x8000u && !((ea ^ opc_) & 0xFF00u)) {
                const uint8_t op0 = FAST_PRG(ea);
                if (op0 == 0x88u || op0 == 0xCAu) {
                    const uint8_t r = (op0 == 0x88u) ? y : x;
                    const uint8_t nz = (uint8_t)((r ? 0 : FAKE6502_ZERO_FLAG) | (r & FAKE6502_SIGN_FLAG));
                    if ((fl & (FAKE6502_ZERO_FLAG | FAKE6502_SIGN_FLAG)) == nz && r > 1u) {
                        const int now_ = clk + ticks;
                        int nmax = r - 1;
                        const int n_dl = (int)((uint32_t)dl_ - (uint32_t)now_ - 9u) / 5;
                        const int n_lim = (int)((uint32_t)lim_ - (uint32_t)now_ - 1u) / 5;
                        if ((int32_t)((uint32_t)dl_ - (uint32_t)now_) < 9) nmax = 0;
                        if ((int32_t)((uint32_t)lim_ - (uint32_t)now_) < 1) nmax = 0;
                        if (n_dl < nmax) nmax = n_dl;
                        if (n_lim < nmax) nmax = n_lim;
                        if (nmax > 0) {
                            const uint8_t r2 = (uint8_t)(r - nmax);
                            if (op0 == 0x88u) y = r2; else x = r2;
                            NZ(r2);
                            ticks += 5 * nmax; n += 2 * nmax;
                            g_ff_cycles_total += (unsigned long)(5 * nmax);
                        }
                    }
                }
            }
#endif
        }
        instr_done:
        clk += ticks; n++;
        if (irq_) { pend = (uint8_t)(!old_i); if (pend) goto done; }
        if (ffs_ != 0 && pc <= ipc) {
            /* Surucuye (bos dongu / yoklama adayi) yalniz gerektiginde don: (a) bu cagri bir yavas komutun hemen ardindan
             * basladiysa ilk geri dallanmada ($2002 yoklama donguleri: her tur bir yavas okuma icerir); (b) ayni hedefe,
             * ayni yazmaclarla ve arada hicbir yazma / PRG-RAM okumasi olmadan ikinci kez donulduyse (bos dongu olabilir).
             * Diger geri dallanmalarda calismaya devam edilir: aday denetimi yalniz bir verimlilik noktasidir, atlama
             * kararinin dogrulugu (ayni durum + arada yazma/yavas erisim yok) gozlem araligindan bagimsizdir. */
            if (after_slow && !bb_valid) goto done;
            if (bb_valid && pc == bb_pc && a == bb_a && x == bb_x && y == bb_y && s == bb_s && fl == bb_fl && wn == bb_wn) { g_rf_poll_ret = pr_; goto done; }
            pr_ = 0;
            bb_valid = 1; bb_pc = pc; bb_a = a; bb_x = x; bb_y = y; bb_s = s; bb_fl = fl; bb_wn = wn;
        }
        if (clk >= lim_) goto done;
        continue;

        abort_i:
        pc = ipc; s = s0; lv = lv0; lra = lra0;
        /* n > 0 ise onceki hizli komutlarin ardindan surucunun yapacagi komut arasi isler bos islemdir: bekleyen NMI yok
         * (nmi_ varsa lim_ ilk komuttan sonra durdurur), IRQ servis edilmeyecek (pend == 0), OAM DMA yok, clk < g_dl_min
         * (PPU/APU senkronu gerekmez). Bu yuzden komut, surucuye donup yeniden denemek yerine burada fake6502_step ile
         * calistirilir; sonuc, ayri bir step_1 cagrisiyla birebir aynidir. */
        slow_ = (n != 0);
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
    if (slow_) { fake6502_step(c); g_rf_slow_done = 1; }
    return c->emu.clockticks - clk0;
}
