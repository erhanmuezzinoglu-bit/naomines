#include "mapper.h"
#include "nes_rom.h"
#include <string.h>
#include <stdbool.h>

static uint8_t mmc1_shift_reg = 0x10;
static uint8_t mmc1_control = 0x0C;
static uint8_t mmc1_prg_bank = 0;

void mapper_map_prg_pages(nes_bus_t *bus) {
    int total = nes_unrom_prg_banks_count;
    if (total <= 0) return;

    // --- Mapper 0 (NROM) Koruma Mantığı ---
    if (nes_last_mapper_loaded == 0) {
        for (int i = 16; i < 32; i++) bus->read_table[i] = NULL;
        return; 
    }

    // --- Mapper 2 (UNROM) - Castlevania'nın Kullandığı ---
    if (nes_last_mapper_loaded == 2) {
        // $8000-$BFFF: Seçilebilir Banka
        int bank = nes_unrom_current_bank % total;
        for (int i = 0; i < 8; i++) {
            bus->read_table[16 + i] = &nes_unrom_prg_banks[bank][i * 2048];
        }
        // $C000-$FFFF: Sabit Son Banka
        for (int i = 0; i < 8; i++) {
            bus->read_table[24 + i] = &nes_unrom_prg_banks[total - 1][i * 2048];
        }
    }
    
    // --- Mapper 1 (MMC1) ---
    else if (nes_last_mapper_loaded == 1) {
        uint8_t prg_mode = (mmc1_control >> 2) & 0x03;
        uint8_t bank = mmc1_prg_bank % total;

        if (prg_mode <= 1) { // 32KB Mode
            bank &= 0xFE;
            for (int i = 0; i < 8; i++) bus->read_table[16 + i] = &nes_unrom_prg_banks[bank][i * 2048];
            int next_bank = (bank + 1) % total;
            for (int i = 0; i < 8; i++) bus->read_table[24 + i] = &nes_unrom_prg_banks[next_bank][i * 2048];
        } 
        else if (prg_mode == 2) { // $8000 Fixed, $C000 Swappable
            for (int i = 0; i < 8; i++) bus->read_table[16 + i] = &nes_unrom_prg_banks[0][i * 2048];
            for (int i = 0; i < 8; i++) bus->read_table[24 + i] = &nes_unrom_prg_banks[bank][i * 2048];
        } 
        else { // $8000 Swappable, $C000 Fixed
            for (int i = 0; i < 8; i++) bus->read_table[16 + i] = &nes_unrom_prg_banks[bank][i * 2048];
            for (int i = 0; i < 8; i++) bus->read_table[24 + i] = &nes_unrom_prg_banks[total - 1][i * 2048];
        }
    }
}

void mapper_write(uint16_t addr, uint8_t value, nes_bus_t *bus) {
    // --- Mapper 2 (UNROM) Yazma İşlemi ---
    if (nes_last_mapper_loaded == 2) {
        if (addr >= 0x8000) {
            nes_unrom_current_bank = value & 0x0F; // İlk 16 bankadan birini seç (Castlevania 8 banka kullanır)
            mapper_map_prg_pages(bus);
        }
    } 
    // --- Mapper 1 (MMC1) Yazma İşlemi ---
    else if (nes_last_mapper_loaded == 1) {
        if (value & 0x80) {
            mmc1_shift_reg = 0x10;
            mmc1_control |= 0x0C;
            mapper_map_prg_pages(bus);
        } else {
            bool complete = (mmc1_shift_reg & 0x01);
            mmc1_shift_reg >>= 1;
            mmc1_shift_reg |= (value & 0x01) << 4;
            if (complete) {
                if (addr <= 0x9FFF) {
                    mmc1_control = mmc1_shift_reg;
                } else if (addr >= 0xE000) {
                    mmc1_prg_bank = mmc1_shift_reg & 0x0F;
                }
                mmc1_shift_reg = 0x10;
                mapper_map_prg_pages(bus);
            }
        }
    }
}