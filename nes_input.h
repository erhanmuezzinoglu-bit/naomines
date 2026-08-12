#ifndef NES_INPUT_H
#define NES_INPUT_H

#include <stdint.h>
#include <naomi/maple.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes_input
{
    /* Controller ($4016) minimal */
    uint8_t pad1_strobe;
    uint8_t pad1_shift;

    jvs_buttons_t buttons;
    int input_valid;

    /* edge detection */
    jvs_buttons_t prev_buttons;
    jvs_buttons_t edge;

    /* --- NES input protokol debug alanları --- */
    uint32_t pad1_read_count;    // Toplam $4016 read sayısı (debug)
    uint8_t pad1_shift_last;     // Son $4016 read öncesi shift (debug)
    uint8_t pad1_shiftbit_last;  // Son $4016 read ile dönen bit (debug)

} nes_input_t;

/* --- DEBUG STAT STRUCT --- */
typedef struct {
    int writes_4016;
    int strobe_1_to_0; // snapshot alınma sayısı
    uint8_t last_shift;
    uint8_t last_value;
} nes_input_dbg_stat_t;

extern nes_input_dbg_stat_t nes_input_dbg_stat;

void nes_input_init(nes_input_t *in);

/* poll maple, update buttons + edge */
void nes_input_poll(nes_input_t *in);

/* $4016 write / read */
void nes_io_write_4016(nes_input_t *in, uint8_t value);
uint8_t nes_io_read_4016(nes_input_t *in);

/* $4017 read stub */
uint8_t nes_io_read_4017(nes_input_t *in);

/* pad1 snapshot (debug için) */
uint8_t nes_input_dbg_pad1_snapshot(const nes_input_t *in);

#ifdef __cplusplus
}
#endif

#endif