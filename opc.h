#ifndef OPC_H
#define OPC_H

#include <stdint.h>

// Adresleme Modu Tipi
typedef enum {
    IMP, IMM, ZP, ZPX, ZPY, ABS, ABSX, ABSY, IND, INDX, INDY, REL, ACC
} AddrMode;

typedef struct {
    const char *mnemonic;
    AddrMode mode;
    uint8_t size;
    const char *flags;
} opcode_info;

extern const opcode_info opcodes[256];

// Kısa mnemonic ismi döndürür
const char *opc_name(uint8_t opcode);

/**
 * Adresleme türüne göre operand stringi hazırlar.
 * Accuracy testlerini bozmamak için const uint8_t* kullanır.
 */
void operand_to_str(char *buf, int mode, uint16_t addr, const uint8_t *rom);

/**
 * NES-Style Disassembler:
 * PC'den başlar, opcode ve operandları işleyip 'out' stringine 
 * nestest-style (PC, Hex, Mnemonic, Operand) çıktı üretir.
 */
void disassemble_6502(const uint8_t *mem, uint16_t pc, char *out, int outlen);

#endif // OPC_H