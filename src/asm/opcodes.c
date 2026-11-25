#include <string.h>

#include "asm.h"

typedef struct {
    const char *name;
    AddrMode mode;
    uint8_t opcode;
} OpEntry;

static const OpEntry TABLE[] = {
    { "ADC", AM_IMM, 0x69 }, { "ADC", AM_ZP, 0x65 }, { "ADC", AM_ZPX, 0x75 },
    { "ADC", AM_ABS, 0x6D }, { "ADC", AM_ABX, 0x7D }, { "ADC", AM_ABY, 0x79 },
    { "ADC", AM_IZX, 0x61 }, { "ADC", AM_IZY, 0x71 },
    { "AND", AM_IMM, 0x29 }, { "AND", AM_ZP, 0x25 }, { "AND", AM_ZPX, 0x35 },
    { "AND", AM_ABS, 0x2D }, { "AND", AM_ABX, 0x3D }, { "AND", AM_ABY, 0x39 },
    { "AND", AM_IZX, 0x21 }, { "AND", AM_IZY, 0x31 },
    { "ASL", AM_ACC, 0x0A }, { "ASL", AM_ZP, 0x06 }, { "ASL", AM_ZPX, 0x16 },
    { "ASL", AM_ABS, 0x0E }, { "ASL", AM_ABX, 0x1E },
    { "BCC", AM_REL, 0x90 },
    { "BCS", AM_REL, 0xB0 },
    { "BEQ", AM_REL, 0xF0 },
    { "BIT", AM_ZP, 0x24 }, { "BIT", AM_ABS, 0x2C },
    { "BMI", AM_REL, 0x30 },
    { "BNE", AM_REL, 0xD0 },
    { "BPL", AM_REL, 0x10 },
    { "BRK", AM_IMP, 0x00 },
    { "BVC", AM_REL, 0x50 },
    { "BVS", AM_REL, 0x70 },
    { "CLC", AM_IMP, 0x18 },
    { "CLD", AM_IMP, 0xD8 },
    { "CLI", AM_IMP, 0x58 },
    { "CLV", AM_IMP, 0xB8 },
    { "CMP", AM_IMM, 0xC9 }, { "CMP", AM_ZP, 0xC5 }, { "CMP", AM_ZPX, 0xD5 },
    { "CMP", AM_ABS, 0xCD }, { "CMP", AM_ABX, 0xDD }, { "CMP", AM_ABY, 0xD9 },
    { "CMP", AM_IZX, 0xC1 }, { "CMP", AM_IZY, 0xD1 },
    { "CPX", AM_IMM, 0xE0 }, { "CPX", AM_ZP, 0xE4 }, { "CPX", AM_ABS, 0xEC },
    { "CPY", AM_IMM, 0xC0 }, { "CPY", AM_ZP, 0xC4 }, { "CPY", AM_ABS, 0xCC },
    { "DEC", AM_ZP, 0xC6 }, { "DEC", AM_ZPX, 0xD6 }, { "DEC", AM_ABS, 0xCE },
    { "DEC", AM_ABX, 0xDE },
    { "DEX", AM_IMP, 0xCA },
    { "DEY", AM_IMP, 0x88 },
    { "EOR", AM_IMM, 0x49 }, { "EOR", AM_ZP, 0x45 }, { "EOR", AM_ZPX, 0x55 },
    { "EOR", AM_ABS, 0x4D }, { "EOR", AM_ABX, 0x5D }, { "EOR", AM_ABY, 0x59 },
    { "EOR", AM_IZX, 0x41 }, { "EOR", AM_IZY, 0x51 },
    { "INC", AM_ZP, 0xE6 }, { "INC", AM_ZPX, 0xF6 }, { "INC", AM_ABS, 0xEE },
    { "INC", AM_ABX, 0xFE },
    { "INX", AM_IMP, 0xE8 },
    { "INY", AM_IMP, 0xC8 },
    { "JMP", AM_ABS, 0x4C }, { "JMP", AM_IND, 0x6C },
    { "JSR", AM_ABS, 0x20 },
    { "LDA", AM_IMM, 0xA9 }, { "LDA", AM_ZP, 0xA5 }, { "LDA", AM_ZPX, 0xB5 },
    { "LDA", AM_ABS, 0xAD }, { "LDA", AM_ABX, 0xBD }, { "LDA", AM_ABY, 0xB9 },
    { "LDA", AM_IZX, 0xA1 }, { "LDA", AM_IZY, 0xB1 },
    { "LDX", AM_IMM, 0xA2 }, { "LDX", AM_ZP, 0xA6 }, { "LDX", AM_ZPY, 0xB6 },
    { "LDX", AM_ABS, 0xAE }, { "LDX", AM_ABY, 0xBE },
    { "LDY", AM_IMM, 0xA0 }, { "LDY", AM_ZP, 0xA4 }, { "LDY", AM_ZPX, 0xB4 },
    { "LDY", AM_ABS, 0xAC }, { "LDY", AM_ABX, 0xBC },
    { "LSR", AM_ACC, 0x4A }, { "LSR", AM_ZP, 0x46 }, { "LSR", AM_ZPX, 0x56 },
    { "LSR", AM_ABS, 0x4E }, { "LSR", AM_ABX, 0x5E },
    { "NOP", AM_IMP, 0xEA },
    { "ORA", AM_IMM, 0x09 }, { "ORA", AM_ZP, 0x05 }, { "ORA", AM_ZPX, 0x15 },
    { "ORA", AM_ABS, 0x0D }, { "ORA", AM_ABX, 0x1D }, { "ORA", AM_ABY, 0x19 },
    { "ORA", AM_IZX, 0x01 }, { "ORA", AM_IZY, 0x11 },
    { "PHA", AM_IMP, 0x48 },
    { "PHP", AM_IMP, 0x08 },
    { "PLA", AM_IMP, 0x68 },
    { "PLP", AM_IMP, 0x28 },
    { "ROL", AM_ACC, 0x2A }, { "ROL", AM_ZP, 0x26 }, { "ROL", AM_ZPX, 0x36 },
    { "ROL", AM_ABS, 0x2E }, { "ROL", AM_ABX, 0x3E },
    { "ROR", AM_ACC, 0x6A }, { "ROR", AM_ZP, 0x66 }, { "ROR", AM_ZPX, 0x76 },
    { "ROR", AM_ABS, 0x6E }, { "ROR", AM_ABX, 0x7E },
    { "RTI", AM_IMP, 0x40 },
    { "RTS", AM_IMP, 0x60 },
    { "SBC", AM_IMM, 0xE9 }, { "SBC", AM_ZP, 0xE5 }, { "SBC", AM_ZPX, 0xF5 },
    { "SBC", AM_ABS, 0xED }, { "SBC", AM_ABX, 0xFD }, { "SBC", AM_ABY, 0xF9 },
    { "SBC", AM_IZX, 0xE1 }, { "SBC", AM_IZY, 0xF1 },
    { "SEC", AM_IMP, 0x38 },
    { "SED", AM_IMP, 0xF8 },
    { "SEI", AM_IMP, 0x78 },
    { "STA", AM_ZP, 0x85 }, { "STA", AM_ZPX, 0x95 }, { "STA", AM_ABS, 0x8D },
    { "STA", AM_ABX, 0x9D }, { "STA", AM_ABY, 0x99 }, { "STA", AM_IZX, 0x81 },
    { "STA", AM_IZY, 0x91 },
    { "STX", AM_ZP, 0x86 }, { "STX", AM_ZPY, 0x96 }, { "STX", AM_ABS, 0x8E },
    { "STY", AM_ZP, 0x84 }, { "STY", AM_ZPX, 0x94 }, { "STY", AM_ABS, 0x8C },
    { "TAX", AM_IMP, 0xAA },
    { "TAY", AM_IMP, 0xA8 },
    { "TSX", AM_IMP, 0xBA },
    { "TXA", AM_IMP, 0x8A },
    { "TXS", AM_IMP, 0x9A },
    { "TYA", AM_IMP, 0x98 },
};

/* 65C02 additions. merlin32 accepts these without an XC directive. */
static const OpEntry TABLE_65C02[] = {
    { "ADC", AM_IZP, 0x72 },
    { "AND", AM_IZP, 0x32 },
    { "BIT", AM_IMM, 0x89 }, { "BIT", AM_ZPX, 0x34 }, { "BIT", AM_ABX, 0x3C },
    { "BRA", AM_REL, 0x80 },
    { "CMP", AM_IZP, 0xD2 },
    { "DEC", AM_ACC, 0x3A },
    { "EOR", AM_IZP, 0x52 },
    { "INC", AM_ACC, 0x1A },
    { "JMP", AM_IAX, 0x7C },
    { "LDA", AM_IZP, 0xB2 },
    { "ORA", AM_IZP, 0x12 },
    { "PHX", AM_IMP, 0xDA },
    { "PHY", AM_IMP, 0x5A },
    { "PLX", AM_IMP, 0xFA },
    { "PLY", AM_IMP, 0x7A },
    { "SBC", AM_IZP, 0xF2 },
    { "STA", AM_IZP, 0x92 },
    { "STP", AM_IMP, 0xDB },
    { "STZ", AM_ZP, 0x64 }, { "STZ", AM_ZPX, 0x74 }, { "STZ", AM_ABS, 0x9C },
    { "STZ", AM_ABX, 0x9E },
    { "TRB", AM_ZP, 0x14 }, { "TRB", AM_ABS, 0x1C },
    { "TSB", AM_ZP, 0x04 }, { "TSB", AM_ABS, 0x0C },
    { "WAI", AM_IMP, 0xCB },
};

#define TABLE_COUNT(t) (sizeof(t) / sizeof((t)[0]))

bool opcode_known(const char *mnem)
{
    size_t i;

    for (i = 0; i < TABLE_COUNT(TABLE); i++) {
        if (strcmp(TABLE[i].name, mnem) == 0) {
            return true;
        }
    }
    for (i = 0; i < TABLE_COUNT(TABLE_65C02); i++) {
        if (strcmp(TABLE_65C02[i].name, mnem) == 0) {
            return true;
        }
    }
    return false;
}

bool opcode_lookup(const char *mnem, AddrMode mode, uint8_t *opcode)
{
    size_t i;

    for (i = 0; i < TABLE_COUNT(TABLE); i++) {
        if (TABLE[i].mode == mode && strcmp(TABLE[i].name, mnem) == 0) {
            *opcode = TABLE[i].opcode;
            return true;
        }
    }
    for (i = 0; i < TABLE_COUNT(TABLE_65C02); i++) {
        if (TABLE_65C02[i].mode == mode && strcmp(TABLE_65C02[i].name, mnem) == 0) {
            *opcode = TABLE_65C02[i].opcode;
            return true;
        }
    }
    return false;
}
