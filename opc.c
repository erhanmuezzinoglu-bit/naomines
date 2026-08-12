#include "opc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// NES 6502 için legal+illegal opcode tablosu.
// AccuracyCoin ve Nestest standartlarına göre korunmuştur.
const opcode_info opcodes[256] = {
    // 0x00-0x0F
    {"BRK",  IMP,  1, ""},    {"ORA",  INDX, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"SLO",  INDX, 2, "NZC"},
    {"NOP",  ZP,   2, ""},    {"ORA",  ZP,   2, "NZ"},  {"ASL",  ZP,   2, "NZC"}, {"SLO",  ZP,   2, "NZC"},
    {"PHP",  IMP,  1, ""},    {"ORA",  IMM,  2, "NZ"},  {"ASL",  ACC,  1, "NZC"}, {"ANC",  IMM,  2, "NZC"},
    {"NOP",  ABS,  3, ""},    {"ORA",  ABS,  3, "NZ"},  {"ASL",  ABS,  3, "NZC"}, {"SLO",  ABS,  3, "NZC"},

    // 0x10-0x1F
    {"BPL",  REL,  2, ""},    {"ORA",  INDY, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"SLO",  INDY, 2, "NZC"},
    {"NOP",  ZPX,  2, ""},    {"ORA",  ZPX,  2, "NZ"},  {"ASL",  ZPX,  2, "NZC"}, {"SLO",  ZPX,  2, "NZC"},
    {"CLC",  IMP,  1, "C"},   {"ORA",  ABSY, 3, "NZ"},  {"NOP",  IMP,  1, ""},    {"SLO",  ABSY, 3, "NZC"},
    {"NOP",  ABSX, 3, ""},    {"ORA",  ABSX, 3, "NZ"},  {"ASL",  ABSX, 3, "NZC"}, {"SLO",  ABSX, 3, "NZC"},

    // 0x20-0x2F
    {"JSR",  ABS,  3, ""},    {"AND",  INDX, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"RLA",  INDX, 2, "NZC"},
    {"BIT",  ZP,   2, "NVZ"}, {"AND",  ZP,   2, "NZ"},  {"ROL",  ZP,   2, "NZC"}, {"RLA",  ZP,   2, "NZC"},
    {"PLP",  IMP,  1, ""},    {"AND",  IMM,  2, "NZ"},  {"ROL",  ACC,  1, "NZC"}, {"ANC",  IMM,  2, "NZC"},
    {"BIT",  ABS,  3, "NVZ"}, {"AND",  ABS,  3, "NZ"},  {"ROL",  ABS,  3, "NZC"}, {"RLA",  ABS,  3, "NZC"},

    // 0x30-0x3F
    {"BMI",  REL,  2, ""},    {"AND",  INDY, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"RLA",  INDY, 2, "NZC"},
    {"NOP",  ZPX,  2, ""},    {"AND",  ZPX,  2, "NZ"},  {"ROL",  ZPX,  2, "NZC"}, {"RLA",  ZPX,  2, "NZC"},
    {"SEC",  IMP,  1, "C"},   {"AND",  ABSY, 3, "NZ"},  {"NOP",  IMP,  1, ""},    {"RLA",  ABSY, 3, "NZC"},
    {"NOP",  ABSX, 3, ""},    {"AND",  ABSX, 3, "NZ"},  {"ROL",  ABSX, 3, "NZC"}, {"RLA",  ABSX, 3, "NZC"},

    // 0x40-0x4F
    {"RTI",  IMP,  1, ""},    {"EOR",  INDX, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"SRE",  INDX, 2, "NZC"},
    {"NOP",  ZP,   2, ""},    {"EOR",  ZP,   2, "NZ"},  {"LSR",  ZP,   2, "NZC"}, {"SRE",  ZP,   2, "NZC"},
    {"PHA",  IMP,  1, ""},    {"EOR",  IMM,  2, "NZ"},  {"LSR",  ACC,  1, "NZC"}, {"ALR",  IMM,  2, "NZC"},
    {"JMP",  ABS,  3, ""},    {"EOR",  ABS,  3, "NZ"},  {"LSR",  ABS,  3, "NZC"}, {"SRE",  ABS,  3, "NZC"},

    // 0x50-0x5F
    {"BVC",  REL,  2, ""},    {"EOR",  INDY, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"SRE",  INDY, 2, "NZC"},
    {"NOP",  ZPX,  2, ""},    {"EOR",  ZPX,  2, "NZ"},  {"LSR",  ZPX,  2, "NZC"}, {"SRE",  ZPX,  2, "NZC"},
    {"CLI",  IMP,  1, "I"},   {"EOR",  ABSY, 3, "NZ"},  {"NOP",  IMP,  1, ""},    {"SRE",  ABSY, 3, "NZC"},
    {"NOP",  ABSX, 3, ""},    {"EOR",  ABSX, 3, "NZ"},  {"LSR",  ABSX, 3, "NZC"}, {"SRE",  ABSX, 3, "NZC"},

    // 0x60-0x6F
    {"RTS",  IMP,  1, ""},    {"ADC",  INDX, 2, "NVZC"},{"JAM",  IMP,  1, ""},    {"RRA",  INDX, 2, "NVZC"},
    {"NOP",  ZP,   2, ""},    {"ADC",  ZP,   2, "NVZC"},{"ROR",  ZP,   2, "NZC"}, {"RRA",  ZP,   2, "NVZC"},
    {"PLA",  IMP,  1, "NZ"},  {"ADC",  IMM,  2, "NVZC"},{"ROR",  ACC,  1, "NZC"}, {"ARR",  IMM,  2, "NVZC"},
    {"JMP",  IND,  3, ""},    {"ADC",  ABS,  3, "NVZC"},{"ROR",  ABS,  3, "NZC"}, {"RRA",  ABS,  3, "NVZC"},

    // 0x70-0x7F
    {"BVS",  REL,  2, ""},    {"ADC",  INDY, 2, "NVZC"},{"JAM",  IMP,  1, ""},    {"RRA",  INDY, 2, "NVZC"},
    {"NOP",  ZPX,  2, ""},    {"ADC",  ZPX,  2, "NVZC"},{"ROR",  ZPX,  2, "NZC"}, {"RRA",  ZPX,  2, "NVZC"},
    {"SEI",  IMP,  1, "I"},   {"ADC",  ABSY, 3, "NVZC"},{"NOP",  IMP,  1, ""},    {"RRA",  ABSY, 3, "NVZC"},
    {"NOP",  ABSX, 3, ""},    {"ADC",  ABSX, 3, "NVZC"},{"ROR",  ABSX, 3, "NZC"}, {"RRA",  ABSX, 3, "NVZC"},

    // 0x80-0x8F
    {"NOP",  IMM,  2, ""},    {"STA",  INDX, 2, ""},    {"NOP",  IMM,  2, ""},    {"SAX",  INDX, 2, ""},
    {"STY",  ZP,   2, ""},    {"STA",  ZP,   2, ""},    {"STX",  ZP,   2, ""},    {"SAX",  ZP,   2, ""},
    {"DEY",  IMP,  1, "NZ"},  {"NOP",  IMM,  2, ""},    {"TXA",  IMP,  1, "NZ"},  {"XAA",  IMM,  2, "NZ"}, 
    {"STY",  ABS,  3, ""},    {"STA",  ABS,  3, ""},    {"STX",  ABS,  3, ""},    {"SAX",  ABS,  3, ""},

    // 0x90-0x9F
    {"BCC",  REL,  2, ""},    {"STA",  INDY, 2, ""},    {"JAM",  IMP,  1, ""},    {"AHX",  INDY, 2, ""}, 
    {"STY",  ZPX,  2, ""},    {"STA",  ZPX,  2, ""},    {"STX",  ZPY,  2, ""},    {"SAX",  ZPY,  2, ""},
    {"TYA",  IMP,  1, "NZ"},  {"STA",  ABSY, 3, ""},    {"TXS",  IMP,  1, ""},    {"TAS",  ABSY, 3, ""},
    {"SHY",  ABSX, 3, ""},    {"STA",  ABSX, 3, ""},    {"SHX",  ABSY, 3, ""},    {"AHX",  ABSY, 3, ""},

    // 0xA0-0xAF
    {"LDY",  IMM,  2, "NZ"},  {"LDA",  INDX, 2, "NZ"},  {"LDX",  IMM,  2, "NZ"},  {"LAX",  INDX, 2, "NZ"},
    {"LDY",  ZP,   2, "NZ"},  {"LDA",  ZP,   2, "NZ"},  {"LDX",  ZP,   2, "NZ"},  {"LAX",  ZP,   2, "NZ"},
    {"TAY",  IMP,  1, "NZ"},  {"LDA",  IMM,  2, "NZ"},  {"TAX",  IMP,  1, "NZ"},  {"LAX",  IMM,  2, "NZ"}, 
    {"LDY",  ABS,  3, "NZ"},  {"LDA",  ABS,  3, "NZ"},  {"LDX",  ABS,  3, "NZ"},  {"LAX",  ABS,  3, "NZ"},

    // 0xB0-0xBF
    {"BCS",  REL,  2, ""},    {"LDA",  INDY, 2, "NZ"},  {"JAM",  IMP,  1, ""},    {"LAX",  INDY, 2, "NZ"},
    {"LDY",  ZPX,  2, "NZ"},  {"LDA",  ZPX,  2, "NZ"},  {"LDX",  ZPY,  2, "NZ"},  {"LAX",  ZPY,  2, "NZ"},
    {"CLV",  IMP,  1, "V"},   {"LDA",  ABSY, 3, "NZ"},  {"TSX",  IMP,  1, "NZ"},  {"LAS",  ABSY, 3, "NZ"}, 
    {"LDY",  ABSX, 3, "NZ"},  {"LDA",  ABSX, 3, "NZ"},  {"LDX",  ABSY, 3, "NZ"},  {"LAX",  ABSY, 3, "NZ"},

    // 0xC0-0xCF
    {"CPY",  IMM,  2, "NZC"}, {"CMP",  INDX, 2, "NZC"}, {"NOP",  IMM,  2, ""},    {"DCP",  INDX, 2, "NZC"},
    {"CPY",  ZP,   2, "NZC"}, {"CMP",  ZP,   2, "NZC"}, {"DEC",  ZP,   2, "NZC"}, {"DCP",  ZP,   2, "NZC"}, 
    {"INY",  IMP,  1, "NZ"},  {"CMP",  IMM,  2, "NZC"}, {"DEX",  IMP,  1, "NZ"},  {"AXS",  IMM,  2, "NZC"}, 
    {"CPY",  ABS,  3, "NZC"}, {"CMP",  ABS,  3, "NZC"}, {"DEC",  ABS,  3, "NZC"}, {"DCP",  ABS,  3, "NZC"},

    // 0xD0-0xDF
    {"BNE",  REL,  2, ""},    {"CMP",  INDY, 2, "NZC"}, {"JAM",  IMP,  1, ""},    {"DCP",  INDY, 2, "NZC"},
    {"NOP",  ZPX,  2, ""},    {"CMP",  ZPX,  2, "NZC"}, {"DEC",  ZPX,  2, "NZC"}, {"DCP",  ZPX,  2, "NZC"},
    {"CLD",  IMP,  1, "D"},   {"CMP",  ABSY, 3, "NZC"}, {"NOP",  IMP,  1, ""},    {"DCP",  ABSY, 3, "NZC"},
    {"NOP",  ABSX, 3, ""},    {"CMP",  ABSX, 3, "NZC"}, {"DEC",  ABSX, 3, "NZC"}, {"DCP",  ABSX, 3, "NZC"},

    // 0xE0-0xEF
    {"CPX",  IMM,  2, "NZC"}, {"SBC",  INDX, 2, "NVZC"},{"NOP",  IMM,  2, ""},    {"ISC",  INDX, 2, "NVZC"},
    {"CPX",  ZP,   2, "NZC"}, {"SBC",  ZP,   2, "NVZC"}, {"INC",  ZP,   2, "NZC"}, {"ISC",  ZP,   2, "NVZC"},
    {"INX",  IMP,  1, "NZ"},  {"SBC",  IMM,  2, "NVZC"},{"NOP",  IMP,  1, ""},    {"SBC",  IMM,  2, "NVZC"}, 
    {"CPX",  ABS,  3, "NZC"}, {"SBC",  ABS,  3, "NVZC"}, {"INC",  ABS,  3, "NZC"}, {"ISC",  ABS,  3, "NVZC"},

    // 0xF0-0xFF
    {"BEQ",  REL,  2, ""},    {"SBC",  INDY, 2, "NVZC"},{"JAM",  IMP,  1, ""},    {"ISC",  INDY, 2, "NVZC"},
    {"NOP",  ZPX,  2, ""},    {"SBC",  ZPX,  2, "NVZC"}, {"INC",  ZPX,  2, "NZC"}, {"ISC",  ZPX,  2, "NVZC"},
    {"SED",  IMP,  1, "D"},   {"SBC",  ABSY, 3, "NVZC"},{"NOP",  IMP,  1, ""},    {"ISC",  ABSY, 3, "NVZC"},
    {"NOP",  ABSX, 3, ""},    {"SBC",  ABSX, 3, "NVZC"},{"INC",  ABSX, 3, "NZC"}, {"ISC",  ABSX, 3, "NVZC"}
};

const char *opc_name(uint8_t opcode) {
    return opcodes[opcode].mnemonic;
}

// Gözlemci (Observer) Fonksiyonu: Side-effect yaratmaz.
void operand_to_str(char *buf, int mode, uint16_t addr, const uint8_t *rom) {
    switch(mode) {
        case IMP:  strcpy(buf, ""); break;
        case ACC:  strcpy(buf, "A"); break;
        case IMM:  sprintf(buf, "#$%02X", rom[(addr+1)&0xFFFF]); break;
        case ZP:   sprintf(buf, "$%02X", rom[(addr+1)&0xFFFF]); break;
        case ZPX:  sprintf(buf, "$%02X,X", rom[(addr+1)&0xFFFF]); break;
        case ZPY:  sprintf(buf, "$%02X,Y", rom[(addr+1)&0xFFFF]); break;
        case ABS:  sprintf(buf, "$%04X", rom[(addr+1)&0xFFFF] | (rom[(addr+2)&0xFFFF]<<8)); break;
        case ABSX: sprintf(buf, "$%04X,X", rom[(addr+1)&0xFFFF] | (rom[(addr+2)&0xFFFF]<<8)); break;
        case ABSY: sprintf(buf, "$%04X,Y", rom[(addr+1)&0xFFFF] | (rom[(addr+2)&0xFFFF]<<8)); break;
        case IND:  sprintf(buf, "($%04X)", rom[(addr+1)&0xFFFF] | (rom[(addr+2)&0xFFFF]<<8)); break;
        case INDX: sprintf(buf, "($%02X,X)", rom[(addr+1)&0xFFFF]); break;
        case INDY: sprintf(buf, "($%02X),Y", rom[(addr+1)&0xFFFF]); break;
        case REL: {
            int8_t rel = (int8_t)rom[(addr+1)&0xFFFF];
            uint16_t target = (uint16_t)((addr + 2 + rel) & 0xFFFF);
            sprintf(buf, "$%04X", target);
            break;
        }
        default: strcpy(buf, "?"); break;
    }
}

void disassemble_6502(const uint8_t *mem, uint16_t pc, char *out, int outlen) {
    if (!mem || !out) return;

    uint8_t op = mem[pc];
    const opcode_info *info = &opcodes[op];
    char bytes[16] = "";
    char opstring[32] = "";

    // Raw opcode ve operand byte'larını güvenli oku
    if (info->size == 1) {
        sprintf(bytes, "%02X", op);
    } else if (info->size == 2) {
        sprintf(bytes, "%02X %02X", op, mem[(pc+1)&0xFFFF]);
    } else if (info->size == 3) {
        sprintf(bytes, "%02X %02X %02X", op, mem[(pc+1)&0xFFFF], mem[(pc+2)&0xFFFF]);
    }

    operand_to_str(opstring, info->mode, pc, mem);

    // AccuracyCoin ve Nestest uyumlu format
    snprintf(out, outlen, "%04X  %-8s %-4s %-24s",
            pc, bytes, info->mnemonic, opstring);
}