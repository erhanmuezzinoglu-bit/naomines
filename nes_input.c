#include "nes_input.h"
#include <string.h>

static uint8_t pad1_snapshot(
    const nes_input_t *in)
{
    if (!in->input_valid)
        return 0x00;

    jvs_buttons_t b =
        in->buttons;

    uint8_t v =
        0;

    if (b.player1.button1)
        v |= (1 << 0);

    if (b.player1.button2)
        v |= (1 << 1);

    if (b.player1.button3)
        v |= (1 << 2);

    if (b.player1.start)
        v |= (1 << 3);

    if (b.player1.up)
        v |= (1 << 4);

    if (b.player1.down)
        v |= (1 << 5);

    if (b.player1.left)
        v |= (1 << 6);

    if (b.player1.right)
        v |= (1 << 7);

    return v;
}

void nes_input_init(
    nes_input_t *in)
{
    memset(in, 0, sizeof(*in));
}

void nes_input_poll(
    nes_input_t *in)
{
    jvs_buttons_t now;

    memset(&now, 0, sizeof(now));

    maple_request_jvs_buttons(0x01, &now);

    in->prev_buttons = in->buttons;
    in->buttons = now;
    in->input_valid = 1;

    in->edge.player1.up =
        in->buttons.player1.up &&
        !in->prev_buttons.player1.up;

    in->edge.player1.down =
        in->buttons.player1.down &&
        !in->prev_buttons.player1.down;

    in->edge.player1.left =
        in->buttons.player1.left &&
        !in->prev_buttons.player1.left;

    in->edge.player1.right =
        in->buttons.player1.right &&
        !in->prev_buttons.player1.right;

    in->edge.player1.button1 =
        in->buttons.player1.button1 &&
        !in->prev_buttons.player1.button1;

    in->edge.player1.button2 =
        in->buttons.player1.button2 &&
        !in->prev_buttons.player1.button2;

    in->edge.player1.button3 =
        in->buttons.player1.button3 &&
        !in->prev_buttons.player1.button3;

    in->edge.player1.button4 =
        in->buttons.player1.button4 &&
        !in->prev_buttons.player1.button4;

    in->edge.player1.button5 =
        in->buttons.player1.button5 &&
        !in->prev_buttons.player1.button5;

    in->edge.player1.button6 =
        in->buttons.player1.button6 &&
        !in->prev_buttons.player1.button6;

    in->edge.player1.start =
        in->buttons.player1.start &&
        !in->prev_buttons.player1.start;
}

void nes_io_break_4016_read_sequence(
    nes_input_t *in)
{
    if (!in)
        return;

    in->pad1_last_bus_was_4016 = 0;
}

void nes_io_write_4016(
    nes_input_t *in,
    uint8_t value)
{
    if (!in)
        return;

    in->pad1_last_bus_was_4016 = 0;

    uint8_t new_strobe =
        value & 1;

    if (in->pad1_strobe == 1 &&
        new_strobe == 0) {
        in->pad1_shift = pad1_snapshot(in);
    }

    in->pad1_strobe = new_strobe;
}

uint8_t nes_io_read_4016(
    nes_input_t *in)
{
    if (!in)
        return 0x01;

    uint8_t bit;

    if (in->pad1_strobe) {
        bit = pad1_snapshot(in) & 0x01;
    } else if (in->pad1_last_bus_was_4016) {
        bit = in->pad1_last_read_bit;
    } else {
        bit = in->pad1_shift & 0x01;
        in->pad1_shift >>= 1;
        in->pad1_shift |= 0x80;
    }

    in->pad1_last_bus_was_4016 = 1;
    in->pad1_last_read_bit = bit & 0x01;

    return bit & 0x01;
}

uint8_t nes_io_read_4017(
    nes_input_t *in)
{
    nes_io_break_4016_read_sequence(in);
    return 0x00;
}