#include "nes_bus.h"
#include "nes_rom.h"
#include "apu.h"
#include "mapper.h"
#include "emu_driver.h"
#include "exp_config.h"
#include <string.h>
#include <stdbool.h>

static uint16_t g_last_4016_write_instruction_pc = 0;
static uint8_t g_last_4016_write_valid = 0;

static uint8_t nes_bus_read_raw(
    nes_bus_t *bus,
    uint16_t addr
);

static void sync_apu(
    nes_bus_t *bus,
    fake6502_context *ctx)
{
    int current_ticks = ctx->emu.clockticks;
    int elapsed = current_ticks - bus->last_apu_sync_tick;

    if (elapsed > 0) {
        apu_step(&bus->apu, elapsed);
        bus->last_apu_sync_tick = current_ticks;
    }
}

static void sync_ppu(
    nes_bus_t *bus,
    fake6502_context *ctx)
{
    int current_ticks = ctx->emu.clockticks;
    int elapsed = current_ticks - bus->last_ppu_sync_tick;

    if (elapsed > 0) {
        emu_driver_step_ppu_sampled(
            bus->ppu, (uint32_t)(elapsed * 3));
        bus->last_ppu_sync_tick = current_ticks;
    }
}

void nes_bus_init(
    nes_bus_t *bus,
    ppu_t *ppu,
    nes_input_t *in)
{
    memset(bus, 0, sizeof(*bus));
    bus->ppu = ppu;
    bus->in = in;
    bus->open_bus_value = 0x00;
    bus->last_apu_sync_tick = 0;
    bus->last_ppu_sync_tick = 0;
    bus->oam_dma_pending = 0;
    bus->oam_dma_page = 0;

    g_last_4016_write_instruction_pc = 0;
    g_last_4016_write_valid = 0;

    apu_init(&bus->apu);

    mapper_map_prg_pages(bus);
}

static uint8_t nes_bus_read_raw(
    nes_bus_t *bus,
    uint16_t addr)
{
    if (addr != 0x4016) {
        nes_io_break_4016_read_sequence(bus->in);
    }

    uint8_t data = bus->open_bus_value;

    if (addr < 0x2000) {
        data = bus->mem[addr & 0x07FF];
    }
    else if (addr >= 0x2000 && addr <= 0x3FFF) {
        data = ppu_cpu_read(bus->ppu, addr);
    }
    else if (addr == 0x4016) {
        data = (bus->open_bus_value & 0xE0) |
               (nes_io_read_4016(bus->in) & 0x1F);
    }
    else if (addr == 0x4017) {
        data = (bus->open_bus_value & 0xE0) |
               (nes_io_read_4017(bus->in) & 0x1F);
    }
    else if (addr >= 0x4000 && addr <= 0x4017) {
        data = apu_cpu_read(
            &bus->apu, addr, bus->open_bus_value
        );
    }
    else if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (mapper_prg_ram_enabled(bus)) {
            data = bus->cart_sram[addr - 0x6000];
        }
    }
    else if (addr >= 0x8000) {
        data = bus->mem[addr];
    }

    bus->open_bus_value = data;
    return data;
}

static uint8_t nes_bus_dmc_sample_read(
    nes_bus_t *bus,
    uint16_t sample_addr,
    uint16_t halted_cpu_addr,
    uint8_t *raw_sample_out)
{
    uint8_t saved_controller_sequence = 0;
    uint8_t saved_controller_read_bit = 0;

    if (bus->in) {
        saved_controller_sequence =
            bus->in->pad1_last_bus_was_4016;
        saved_controller_read_bit =
            bus->in->pad1_last_read_bit;
    }

    uint8_t sample =
        nes_bus_read_raw(bus, sample_addr);

    if (raw_sample_out) {
        *raw_sample_out = sample;
    }

    if (bus->in) {
        bus->in->pad1_last_bus_was_4016 =
            saved_controller_sequence;
        bus->in->pad1_last_read_bit =
            saved_controller_read_bit;
    }

    if ((halted_cpu_addr & 0xFFE0) != 0x4000) {
        return sample;
    }

    uint16_t conflict_addr =
        (halted_cpu_addr & 0xFFE0) |
        (sample_addr & 0x001F);

    uint8_t sample_bus_value = sample;
    uint8_t register_value =
        nes_bus_read_raw(bus, conflict_addr);

    if (conflict_addr == 0x4015) {
        bus->open_bus_value = sample_bus_value;
        return sample_bus_value;
    }

    bus->open_bus_value = register_value;
    return register_value;
}

typedef enum {
    OAM_DMA_RUNTIME_HALT = 0,
    OAM_DMA_RUNTIME_ALIGNMENT,
    OAM_DMA_RUNTIME_GET,
    OAM_DMA_RUNTIME_PUT,
    OAM_DMA_RUNTIME_DONE
} oam_dma_runtime_state_t;

typedef enum {
    DMC_DMA_RUNTIME_IDLE = 0,
    DMC_DMA_RUNTIME_DUMMY,
    DMC_DMA_RUNTIME_ALIGNMENT,
    DMC_DMA_RUNTIME_GET
} dmc_dma_runtime_state_t;

static int dma_next_cycle_is_get(
    const fake6502_context *ctx)
{
    return (ctx->emu.clockticks & 1) != 0;
}

static void dma_cycle_advance(
    nes_bus_t *bus,
    fake6502_context *ctx)
{
    ctx->emu.clockticks++;
    sync_ppu(bus, ctx);
    sync_apu(bus, ctx);
}

static void nes_bus_dmc_complete_get(
    nes_bus_t *bus,
    uint16_t halted_addr,
    uint16_t sample_addr)
{
    dmc_t *d = &bus->apu.dmc;
    uint8_t raw_sample = 0;

    uint8_t cpu_visible =
        nes_bus_dmc_sample_read(
            bus, sample_addr, halted_addr, &raw_sample
        );

    d->sample_buffer = raw_sample;
    d->buffer_full = true;
    d->current_address =
        (d->current_address + 1) | 0x8000;

    if (d->current_length > 0) {
        d->current_length--;
        if (d->current_length == 0) {
            if (d->loop) {
                d->current_address = d->sample_address;
                d->current_length = d->sample_length;
            }
            else if (d->irq_enable) {
                d->irq_pending = 1;
                emu_driver_request_irq();
            }
        }
    }

    (void)cpu_visible;
}

static uint8_t nes_bus_oam_source_read(
    nes_bus_t *bus,
    uint16_t source_addr,
    uint16_t parked_cpu_addr)
{
    if (source_addr < 0x4000u &&
        (parked_cpu_addr & 0xFFE0u) == 0x4000u) {

        uint8_t source_value =
            nes_bus_read_raw(bus, source_addr);

        switch (source_addr & 0x001Fu) {
            case 0x15:
                return apu_cpu_read(
                    &bus->apu, 0x4015, source_value
                );

            case 0x16:
                (void)nes_bus_read_raw(bus, 0x4016);
                bus->open_bus_value = source_value;
                return source_value;

            case 0x17:
                (void)nes_bus_read_raw(bus, 0x4017);
                bus->open_bus_value = source_value;
                return source_value;

            default:
                return source_value;
        }
    }

    if (source_addr >= 0x4000 &&
        source_addr <= 0x5FFF) {

        if ((parked_cpu_addr & 0xFFE0u) != 0x4000u) {
            return bus->open_bus_value;
        }

        return nes_bus_read_raw(
            bus,
            (uint16_t)(0x4000u |
                       (source_addr & 0x001Fu))
        );
    }

    return nes_bus_read_raw(bus, source_addr);
}

void nes_bus_oam_dma_step(
    nes_bus_t *bus,
    fake6502_context *ctx)
{
    if (!bus || !ctx || !bus->oam_dma_pending) {
        return;
    }

    uint8_t page = bus->oam_dma_page;
    bus->oam_dma_pending = 0;

    uint16_t base = (uint16_t)page << 8;
    uint16_t parked_cpu_addr = ctx->cpu.pc;

    oam_dma_runtime_state_t oam_state =
        OAM_DMA_RUNTIME_HALT;
    dmc_dma_runtime_state_t dmc_state =
        DMC_DMA_RUNTIME_IDLE;

    uint16_t oam_index = 0;
    uint8_t oam_data = 0;
    uint16_t dmc_sample_addr = 0;

    while (oam_state != OAM_DMA_RUNTIME_DONE ||
           dmc_state != DMC_DMA_RUNTIME_IDLE) {

        dmc_t *d = &bus->apu.dmc;
        int cycle_is_get = dma_next_cycle_is_get(ctx);

        dmc_dma_runtime_state_t dmc_state_before =
            dmc_state;

        int dmc_request_before_cycle =
            dmc_state_before == DMC_DMA_RUNTIME_IDLE &&
            d->dma_request;

        int dmc_halt_started = 0;

        if (dmc_state_before == DMC_DMA_RUNTIME_IDLE &&
            d->dma_request &&
            d->dma_delay == 0) {

            uint8_t kind = d->dma_kind;
            if (kind == APU_DMC_DMA_NONE) {
                kind = APU_DMC_DMA_RELOAD;
            }

            int delayed_parity =
                d->dma_delay_parity != 0;

            int wants_get =
                (kind == APU_DMC_DMA_LOAD)
                    ? !delayed_parity
                    : delayed_parity;

            if ((wants_get != 0) ==
                (cycle_is_get != 0)) {

                dmc_sample_addr = d->current_address;
                d->dma_request = 0;
                d->dma_kind = APU_DMC_DMA_NONE;
                d->dma_delay = 0;
                d->dma_delay_parity = 0;
                dmc_state = DMC_DMA_RUNTIME_DUMMY;
                dmc_halt_started = 1;
            }
        }

        int dmc_get_now =
            dmc_state_before == DMC_DMA_RUNTIME_GET;

        int oam_get_stolen =
            dmc_get_now &&
            oam_state == OAM_DMA_RUNTIME_GET &&
            cycle_is_get;

        dma_cycle_advance(bus, ctx);

        if (dmc_get_now) {
            if (oam_state == OAM_DMA_RUNTIME_PUT) {
                ppu_cpu_write(bus->ppu, 0x2004, oam_data);
            }

            nes_bus_dmc_complete_get(
                bus, parked_cpu_addr, dmc_sample_addr
            );
        }
        else {
            switch (oam_state) {
                case OAM_DMA_RUNTIME_HALT:
                case OAM_DMA_RUNTIME_ALIGNMENT:
                    bus->last_cpu_read_addr = parked_cpu_addr;
                    (void)nes_bus_read_raw(
                        bus, parked_cpu_addr
                    );
                    break;

                case OAM_DMA_RUNTIME_GET:
                    if (!oam_get_stolen) {
                        oam_data =
                            nes_bus_oam_source_read(
                                bus,
                                (uint16_t)(base + oam_index),
                                parked_cpu_addr
                            );
                        bus->last_cpu_read_addr = parked_cpu_addr;
                    }
                    break;

                case OAM_DMA_RUNTIME_PUT:
                    ppu_cpu_write(
                        bus->ppu, 0x2004, oam_data
                    );
                    break;

                case OAM_DMA_RUNTIME_DONE:
                default:
                    break;
            }
        }

        if (oam_state == OAM_DMA_RUNTIME_HALT) {
            oam_state =
                dma_next_cycle_is_get(ctx)
                    ? OAM_DMA_RUNTIME_GET
                    : OAM_DMA_RUNTIME_ALIGNMENT;
        }
        else if (oam_state == OAM_DMA_RUNTIME_ALIGNMENT) {
            oam_state = OAM_DMA_RUNTIME_GET;
        }
        else if (oam_state == OAM_DMA_RUNTIME_GET) {
            if (oam_get_stolen) {
                oam_state = OAM_DMA_RUNTIME_ALIGNMENT;
            }
            else {
                oam_state = OAM_DMA_RUNTIME_PUT;
            }
        }
        else if (oam_state == OAM_DMA_RUNTIME_PUT) {
            oam_index++;
            if (oam_index >= 256) {
                oam_state = OAM_DMA_RUNTIME_DONE;
            }
            else {
                oam_state = OAM_DMA_RUNTIME_GET;
            }
        }

        if (dmc_halt_started) {
            dmc_state = DMC_DMA_RUNTIME_DUMMY;
        }
        else if (dmc_state_before ==
                 DMC_DMA_RUNTIME_DUMMY) {
            dmc_state =
                dma_next_cycle_is_get(ctx)
                    ? DMC_DMA_RUNTIME_GET
                    : DMC_DMA_RUNTIME_ALIGNMENT;
        }
        else if (dmc_state_before ==
                 DMC_DMA_RUNTIME_ALIGNMENT) {
            dmc_state = DMC_DMA_RUNTIME_GET;
        }
        else if (dmc_state_before ==
                 DMC_DMA_RUNTIME_GET) {
            dmc_state = DMC_DMA_RUNTIME_IDLE;
        }

        if (dmc_request_before_cycle &&
            !dmc_halt_started &&
            d->dma_request &&
            d->dma_delay > 0) {
            d->dma_delay--;
        }
    }

    bus->last_cpu_read_addr = parked_cpu_addr;
    bus->last_apu_sync_tick = ctx->emu.clockticks;
    bus->last_ppu_sync_tick = ctx->emu.clockticks;
}

uint8_t nes_bus_read(
    nes_bus_t *bus,
    fake6502_context *ctx,
    uint16_t addr)
{
#if EXP_LAZY_APU
    if (addr >= 0x4000 && addr <= 0x401F) {
        sync_apu(bus, ctx);
    }
#else
    sync_apu(bus, ctx);
#endif
#if EXP_LAZY_PPU
    /* PPU yazmaclari ve kartus yazmac alani: erisimden once PPU'yu guncelle. */
    if ((addr >= 0x2000 && addr <= 0x3FFF) || (addr >= 0x4020 && addr < 0x6000)) {
        sync_ppu(bus, ctx);
    }
#endif
    bus->last_cpu_read_addr = addr;

    if (addr != 0x4016) {
        nes_io_break_4016_read_sequence(bus->in);
    }

    if (addr == 0x4015) {
        uint8_t value =
            apu_cpu_read(
                &bus->apu, addr, bus->cpu_internal_bus_value
            );

        bus->cpu_internal_bus_value = value;
        return value;
    }

    if (addr >= 0x4000 && addr <= 0x4013) {
        uint8_t value = bus->open_bus_value;
        bus->cpu_internal_bus_value = value;
        return value;
    }

    uint8_t value =
        nes_bus_read_raw(bus, addr);

    bus->cpu_internal_bus_value = value;

    if (addr < 0x4000 || addr >= 0x4018) {
        bus->open_bus_value = value;
    }

    return value;
}

void nes_bus_write(
    nes_bus_t *bus,
    fake6502_context *ctx,
    uint16_t addr,
    uint8_t value)
{
    nes_io_break_4016_read_sequence(bus->in);
#if EXP_LAZY_APU
    if (addr >= 0x4000 && addr <= 0x401F) {
        sync_apu(bus, ctx);
    }
#else
    sync_apu(bus, ctx);
#endif
#if EXP_LAZY_PPU
    /* PPU yazmaclari ve TUM kartus yazmaclari (CHR bank degisimi PPU'yu etkiler). */
    if (addr >= 0x2000 && (addr <= 0x3FFF || addr >= 0x4020)) {
        sync_ppu(bus, ctx);
    }
#endif
    bus->open_bus_value = value;
    bus->cpu_internal_bus_value = value;

    if (addr < 0x2000) {
        bus->mem[addr & 0x07FF] = value;
    }
    else if (addr >= 0x2000 && addr <= 0x3FFF) {
        uint16_t reg = 0x2000 | (addr & 0x0007);
        ppu_cpu_write(bus->ppu, reg, value);
    }
    else if (addr == 0x4014) {
        bus->oam_dma_page = value;
        bus->oam_dma_pending = 1;
    }
    else if (addr == 0x4016) {
        bool same_instruction =
            g_last_4016_write_valid &&
            g_last_4016_write_instruction_pc ==
                ctx->emu.instruction_pc;

        bool skip_write =
            bus->in &&
            same_instruction &&
            (value & 0x01u) == 0 &&
            bus->in->pad1_strobe &&
            !(ctx->emu.clockticks & 1);

        if (skip_write) {
            bus->in->pad1_strobe = 0;
        }
        else {
            nes_io_write_4016(bus->in, value);
        }

        g_last_4016_write_instruction_pc =
            ctx->emu.instruction_pc;
        g_last_4016_write_valid = 1;
    }
    else if (addr >= 0x4000 && addr <= 0x4017) {
        apu_cpu_write(&bus->apu, addr, value);
    }
    else if (addr >= 0x5000 && addr <= 0x5FFF) {
        mapper_write(addr, value, bus);
    }
    else if (addr >= 0x6000 && addr <= 0x7FFF) {
        if (mapper_prg_ram_write_enabled(bus)) {
            bus->cart_sram[addr - 0x6000] = value;
        }
    }
    else if (addr >= 0x8000) {
        mapper_write(addr, value, bus);
    }
}

void nes_bus_dma_dmc_step(
    nes_bus_t *bus,
    fake6502_context *ctx)
{
    if (!bus || !ctx) return;

    dmc_t *d = &bus->apu.dmc;
    if (!d->dma_request) return;
    if (d->dma_delay != 0) return;

    uint8_t dma_kind = d->dma_kind;
    uint8_t delayed_parity = d->dma_delay_parity;

    if ((delayed_parity & 0xFEu) ==
        DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE) {
        delayed_parity &= 0x01u;
    }

    int dma_cycles;

    if (dma_kind == APU_DMC_DMA_LOAD) {
        dma_cycles = delayed_parity ? 4 : 3;
    }
    else {
        dma_cycles = delayed_parity ? 3 : 4;
    }

    d->dma_request = 0;
    d->dma_kind = APU_DMC_DMA_NONE;
    d->dma_delay = 0;

    if ((d->dma_delay_parity & 0xFEu) ==
        DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE) {
    }
    else {
        d->dma_delay_parity = DMC_DMA_RUNTIME_ACTIVE;
    }

    uint16_t halted_addr = bus->last_cpu_read_addr;
    uint16_t sample_addr = d->current_address;

    for (int i = 0; i < dma_cycles - 1; i++) {
        ctx->emu.clockticks++;
        sync_ppu(bus, ctx);
        sync_apu(bus, ctx);

        if (d->dma_delay_parity ==
            DMC_DMA_RUNTIME_IMPLICIT_ACTIVE) {

            if (i == 0) {
                d->dma_delay_parity = 0;
                bus->last_cpu_read_addr = halted_addr;
                bus->last_apu_sync_tick = ctx->emu.clockticks;
                bus->last_ppu_sync_tick = ctx->emu.clockticks;
                return;
            }
        }
        else if (d->dma_delay_parity ==
                     DMC_DMA_RUNTIME_ABORT ||
                 d->dma_delay_parity ==
                     DMC_DMA_RUNTIME_IMPLICIT_ABORT) {

            if (i == 0) {
                d->dma_delay_parity = 0;
                bus->last_cpu_read_addr = halted_addr;
                bus->last_apu_sync_tick = ctx->emu.clockticks;
                bus->last_ppu_sync_tick = ctx->emu.clockticks;
                return;
            }

            d->dma_delay_parity =
                DMC_DMA_RUNTIME_ACTIVE;
        }

        bus->last_cpu_read_addr = halted_addr;
        (void)nes_bus_read_raw(bus, halted_addr);
    }

    ctx->emu.clockticks++;
    sync_ppu(bus, ctx);
    sync_apu(bus, ctx);

    if (d->dma_delay_parity ==
        DMC_DMA_RUNTIME_ABORT) {
        d->dma_delay_parity =
            DMC_DMA_RUNTIME_ACTIVE;
    }
    else if (d->dma_delay_parity ==
             DMC_DMA_RUNTIME_IMPLICIT_ABORT) {
        d->dma_delay_parity =
            DMC_DMA_RUNTIME_IMPLICIT_ACTIVE;
    }

    uint8_t raw_sample = 0;
    uint8_t cpu_visible =
        nes_bus_dmc_sample_read(
            bus, sample_addr, halted_addr,
            &raw_sample
        );

    (void)cpu_visible;

    if (d->current_length > 0) {
        d->sample_buffer = raw_sample;
        d->buffer_full = true;

        d->current_address =
            (d->current_address + 1) | 0x8000;
        d->current_length--;

        if (d->current_length == 0) {
            if (d->loop) {
                d->current_address = d->sample_address;
                d->current_length = d->sample_length;
            }
            else if (d->irq_enable) {
                d->irq_pending = 1;
                emu_driver_request_irq();
            }
        }
    }

    if (d->sample_length == 1 &&
        !d->loop &&
        d->bit_count == 1 &&
        d->timer < 2) {
        d->current_address = d->sample_address;
        d->current_length = 1;
        d->buffer_full = true;
        d->dma_delay_parity =
            DMC_DMA_RUNTIME_IMPLICIT_PENDING_BASE;
        apu_dmc_arm_implicit_abort(&bus->apu);
    }

    bus->last_cpu_read_addr = halted_addr;
    bus->last_apu_sync_tick = ctx->emu.clockticks;
    bus->last_ppu_sync_tick = ctx->emu.clockticks;
}