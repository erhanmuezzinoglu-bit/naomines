#include "nes_bus.h"
#include "nes_rom.h"
#include "apu.h" 
#include "mapper.h"
#include "emu_driver.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static void sync_apu(nes_bus_t *bus, fake6502_context *ctx) {
    int current_ticks = ctx->emu.clockticks;
    int elapsed = current_ticks - bus->last_apu_sync_tick;
    if (elapsed > 0) {
        apu_step(&bus->apu, elapsed);
        bus->last_apu_sync_tick = current_ticks;
    }
}

static void sync_ppu(nes_bus_t *bus, fake6502_context *ctx) {
    int current_ticks = ctx->emu.clockticks;
    int elapsed = current_ticks - bus->last_ppu_sync_tick;
    if (elapsed > 0) {
        ppu_step_cycles(bus->ppu, elapsed * 3);
        bus->last_ppu_sync_tick = current_ticks;
    }
}

void nes_bus_init(nes_bus_t *bus, ppu_t *ppu, nes_input_t *in, trace_state_t *trace) {
    memset(bus, 0, sizeof(*bus));
    bus->ppu = ppu;
    bus->in = in;
    bus->trace = trace;
    bus->open_bus_value = 0x00;
    bus->last_apu_sync_tick = 0;
    apu_init(&bus->apu);

    for (int i = 0; i < 32; i++) {
        bus->read_table[i] = NULL;
    }

    mapper_map_prg_pages(bus);
}

static uint8_t nes_bus_read_no_trace(nes_bus_t *bus, uint16_t addr) {
    uint8_t data = bus->open_bus_value;

    if (addr < 0x2000) {
        data = bus->mem[addr & 0x07FF];
    }
    else if (addr >= 0x2000 && addr <= 0x3FFF) {
        data = ppu_cpu_read(bus->ppu, addr); 
    }
    else if (addr == 0x4016) {
        // Joypad 1: Üst 3 bit Open Bus olmalı
        data = (bus->open_bus_value & 0xE0) | (nes_io_read_4016(bus->in) & 0x1F);
    }
    else if (addr == 0x4017) {
        // Joypad 2: Üst 3 bit Open Bus olmalı
        data = (bus->open_bus_value & 0xE0) | (nes_io_read_4017(bus->in) & 0x1F);
    }
    else if (addr >= 0x4000 && addr <= 0x4017) {
        // APU okumaları (Özellikle $4015)
        // apu_cpu_read fonksiyonu Open Bus değerini parametre olarak almalı
        data = apu_cpu_read(&bus->apu, addr, bus->open_bus_value);
    }
    else if (addr >= 0x6000 && addr <= 0x7FFF) {
        data = bus->cart_sram[addr - 0x6000];
    }
    else if (addr >= 0x8000) {
        uint8_t *ptr = bus->read_table[addr >> 11];
        if (ptr) {
            data = ptr[addr & 0x07FF];
        } else {
            data = bus->mem[addr];
        }
    }

    bus->open_bus_value = data;
    return data;
}

uint8_t nes_bus_read(nes_bus_t *bus, fake6502_context *ctx, uint16_t addr) {
    sync_apu(bus, ctx);

    // CPU'nun en son okuduğu adresi kaydet (DMC DMA halt/alignment dummy read için).
    // NOT: $2007 DMA tetiklemesi artık fake6502_mem_read içinde yapılıyor
    // (last_cpu_read_addr doğru sırada ayarlanıyor). Burada tekrar tetiklenmez;
    // yalnızca adres kaydı tutulur.
    bus->last_cpu_read_addr = addr;

    // KRİTİK DÜZELTME: $4015 okuması databus'ı (open_bus_value) güncellememeli!
    // Test 7 bunu kontrol ediyor. Fonksiyon sonucu döner ama internal bus değişmez.
    if (addr == 0x4015) {
        uint8_t value = apu_cpu_read(&bus->apu, addr, bus->open_bus_value);
        if (bus->trace) trace_push(bus->trace, ctx, addr, value, 0, 0);
        // bus->open_bus_value = value; <-- BU SATIR KALDIRILDI!
        return value;
    }

    // Yazma amaçlı registerlar okunduğunda Open Bus döner, bus güncellenmez
    if (addr >= 0x4000 && addr <= 0x4013) {
        uint8_t value = bus->open_bus_value;
        if (bus->trace) trace_push(bus->trace, ctx, addr, value, 0, 0);
        return value;
    }

    uint8_t value = nes_bus_read_no_trace(bus, addr);

    if (addr == 0x4016) bus->dbg_4016_reads++;
    if (bus->trace) trace_push(bus->trace, ctx, addr, value, 0, 0);

    // Standart bellek okumaları bus değerini günceller
    if (addr < 0x4000 || addr >= 0x4018) {
        bus->open_bus_value = value;
    }
    
    return value;
}

void nes_bus_write(nes_bus_t *bus, fake6502_context *ctx, uint16_t addr, uint8_t value) {
    sync_apu(bus, ctx);

    // Her yazma işlemi veri yolundaki değeri günceller
    bus->open_bus_value = value; 

    if (addr < 0x2000) {
        bus->mem[addr & 0x07FF] = value;
    }
    else if (addr >= 0x2000 && addr <= 0x3FFF) {
        uint16_t reg = 0x2000 | (addr & 0x0007);
        ppu_cpu_write(bus->ppu, reg, value);
        if (reg == 0x2000) bus->watch_ppu_2000 = value; 
        if (reg == 0x2001) bus->watch_ppu_2001 = value;
        if (reg == 0x2005) { bus->watch_ppu_2005_a = bus->watch_ppu_2005_b; bus->watch_ppu_2005_b = value; }
        if (reg == 0x2006) { bus->watch_ppu_2006_hi = bus->watch_ppu_2006_lo; bus->watch_ppu_2006_lo = value; }
    }
    else if (addr == 0x4014) {
        uint16_t base = (uint16_t)value << 8;
        int dma_cycles = (ctx->emu.clockticks & 1) ? 514 : 513;
        
        sync_ppu(bus, ctx);
        ctx->emu.clockticks += dma_cycles;
        ppu_step_cycles(bus->ppu, dma_cycles * 3);
        bus->last_ppu_sync_tick = ctx->emu.clockticks;
        apu_step(&bus->apu, dma_cycles);
        bus->last_apu_sync_tick = ctx->emu.clockticks;
        
        for (int i = 0; i < 256; i++) {
            uint8_t dma_val = nes_bus_read_no_trace(bus, base + i);
            ppu_cpu_write(bus->ppu, 0x2004, dma_val);
        }
        bus->dbg_oam_dma_count++;
    }
    else if (addr == 0x4016) {
        nes_io_write_4016(bus->in, value);
    }
    else if (addr >= 0x4000 && addr <= 0x4017) {
        apu_cpu_write(&bus->apu, addr, value);
    }
    else if (addr >= 0x6000 && addr <= 0x7FFF) {
        bus->cart_sram[addr - 0x6000] = value;
    }
    else if (addr >= 0x8000) {
        mapper_write(addr, value, bus);
    }

    if (bus->trace) trace_push(bus->trace, ctx, addr, value, 1, 0);
}

void nes_bus_dma_dmc_step(nes_bus_t *bus, fake6502_context *ctx) {
    if (bus->apu.dmc.dma_request) {
        int dma_cycles = (ctx->emu.clockticks & 1) ? 3 : 4;
        // halt + alignment cycle sayısı: toplam cycle - 1 (get cycle hariç)
        int halt_cycles = dma_cycles - 1;
        
        sync_ppu(bus, ctx);
        ctx->emu.clockticks += dma_cycles;
        ppu_step_cycles(bus->ppu, dma_cycles * 3);
        bus->last_ppu_sync_tick = ctx->emu.clockticks;
        
        apu_step(&bus->apu, dma_cycles);
        bus->last_apu_sync_tick = ctx->emu.clockticks;

        // Halt/alignment cycle'larında CPU'nun son okuduğu adresten dummy read yap.
        // Bu, $2002/$2007/$4015/$4016 gibi yan etkili register'ları doğru tetikler:
        //   $4015 okunursa APU Frame Counter IRQ flag'i temizlenir,
        //   $2007 okunursa PPU read buffer ve v register'ı güncellenir,
        //   $4016 okunursa controller port clock'lanır.
        for (int i = 0; i < halt_cycles; i++) {
            nes_bus_read_no_trace(bus, bus->last_cpu_read_addr);
        }

        // Get cycle: DMC sample adresinden oku.
        // nes_bus_read_no_trace $4000-$4017 aralığı için apu_cpu_read çağırır,
        // bu sayede $4015 bus conflict APU Frame Counter IRQ flag'ini temizler.
        uint16_t sample_addr = bus->apu.dmc.current_address;
        bus->apu.dmc.sample_buffer = nes_bus_read_no_trace(bus, sample_addr);
        bus->apu.dmc.buffer_full = true;
        
        bus->apu.dmc.current_address = (bus->apu.dmc.current_address + 1) | 0x8000;
        if (bus->apu.dmc.current_length > 0) {
            bus->apu.dmc.current_length--;
            if (bus->apu.dmc.current_length == 0) {
                if (bus->apu.dmc.loop) {
                    bus->apu.dmc.current_address = bus->apu.dmc.sample_address;
                    bus->apu.dmc.current_length = bus->apu.dmc.sample_length;
                } else if (bus->apu.dmc.irq_enable) {
                    bus->apu.dmc.irq_pending = 1;
                    emu_driver_request_irq();
                }
            }
        }
        
        bus->apu.dmc.dma_request = 0;
    }
}