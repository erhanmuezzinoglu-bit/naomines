#ifndef NES_INPUT_H
#define NES_INPUT_H

#include <stdint.h>
#include <naomi/maple.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes_input
{
    uint8_t pad1_strobe;
    uint8_t pad1_shift;

    jvs_buttons_t buttons;
    int input_valid;

    jvs_buttons_t prev_buttons;
    jvs_buttons_t edge;

    uint8_t pad1_last_bus_was_4016;
    uint8_t pad1_last_read_bit;

} nes_input_t;

void nes_input_init(
    nes_input_t *in
);

void nes_input_poll(
    nes_input_t *in
);

void nes_io_write_4016(
    nes_input_t *in,
    uint8_t value
);

uint8_t nes_io_read_4016(
    nes_input_t *in
);

uint8_t nes_io_read_4017(
    nes_input_t *in
);

void nes_io_break_4016_read_sequence(
    nes_input_t *in
);

#ifdef __cplusplus
}
#endif

#endif