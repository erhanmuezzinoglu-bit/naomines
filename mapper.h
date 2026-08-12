#ifndef MAPPER_H
#define MAPPER_H

#include "nes_bus.h"

// Mapper mantığını yürüten ana fonksiyon (Okuma tablolarını eşler)
void mapper_map_prg_pages(nes_bus_t *bus);

// Kartuş bölgesine ($8000-$FFFF) yapılan yazma işlemlerini yönetir
void mapper_write(uint16_t addr, uint8_t value, nes_bus_t *bus);

#endif