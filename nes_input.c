#include "nes_input.h"
#include <string.h>
#include <stdio.h> 

// Emu driver'dan komut sayacını alıyoruz
extern uint32_t emu_driver_instr_count(void);

nes_input_dbg_stat_t nes_input_dbg_stat = {0}; 

/** NES PAD SNAPSHOT - Sadece NES'in anladığı 8 bit */
static uint8_t pad1_snapshot(const nes_input_t *in)
{
    if (!in->input_valid) return 0x00;

    jvs_buttons_t b = in->buttons;
    uint8_t v = 0;

    // NES Buton Eşlemeleri (Basılan = 1)
    if (b.player1.button1) v |= (1 << 0); /* A */
    if (b.player1.button2) v |= (1 << 1); /* B */
    if (b.player1.button3) v |= (1 << 2); /* Select */
    if (b.player1.start)   v |= (1 << 3); /* Start */
    if (b.player1.up)      v |= (1 << 4);
    if (b.player1.down)    v |= (1 << 5);
    if (b.player1.left)    v |= (1 << 6);
    if (b.player1.right)   v |= (1 << 7);

    return v;
}

void nes_input_init(nes_input_t *in)
{
    memset(in, 0, sizeof(*in));
    memset(&nes_input_dbg_stat, 0, sizeof(nes_input_dbg_stat));
}

void nes_input_poll(nes_input_t *in)
{
    jvs_buttons_t now;
    memset(&now, 0, sizeof(now));

    maple_request_jvs_buttons(0x01, &now);

    // Edge detection (kenar tetikleme) için önceki durumu sakla
    in->prev_buttons = in->buttons;
    in->buttons = now;
    in->input_valid = 1;

    // HUD / GUI Kontrolleri için tüm butonları işle
    in->edge.player1.up      =  in->buttons.player1.up      && !in->prev_buttons.player1.up;
    in->edge.player1.down    =  in->buttons.player1.down    && !in->prev_buttons.player1.down;
    in->edge.player1.left    =  in->buttons.player1.left    && !in->prev_buttons.player1.left;
    in->edge.player1.right   =  in->buttons.player1.right   && !in->prev_buttons.player1.right;
    in->edge.player1.button1 =  in->buttons.player1.button1 && !in->prev_buttons.player1.button1;
    in->edge.player1.button2 =  in->buttons.player1.button2 && !in->prev_buttons.player1.button2;
    in->edge.player1.button3 =  in->buttons.player1.button3 && !in->prev_buttons.player1.button3;
    
    // Senin ROM başlatmak için kullandığın kritik butonlar burada korunuyor:
    in->edge.player1.button4 =  in->buttons.player1.button4 && !in->prev_buttons.player1.button4;
    in->edge.player1.button5 =  in->buttons.player1.button5 && !in->prev_buttons.player1.button5;
    in->edge.player1.button6 =  in->buttons.player1.button6 && !in->prev_buttons.player1.button6;
    
    in->edge.player1.start   =  in->buttons.player1.start   && !in->prev_buttons.player1.start;
}

void nes_io_write_4016(nes_input_t *in, uint8_t value)
{
    uint8_t new_strobe = value & 1;
    
    // 1'den 0'a geçişte (Latch) buton durumlarını dondur
    if (in->pad1_strobe == 1 && new_strobe == 0) {
        // Not: Burada poll çağırmıyoruz, poll ana emülatör döngüsünde zaten çağrılıyor.
        // Sadece mevcut buton durumunun snapshot'ını alıyoruz.
        in->pad1_shift = pad1_snapshot(in);
    }
    in->pad1_strobe = new_strobe;
}

static uint32_t g_last_read_instr = 0xFFFFFFFF;
static uint8_t g_last_read_ret = 0;

uint8_t nes_io_read_4016(nes_input_t *in)
{
    uint8_t bit;
    uint32_t current_instr = emu_driver_instr_count();

    if (in->pad1_strobe) {
        bit = (pad1_snapshot(in) & 0x01);
    } else {
        // Fail 6 & Fail 4 düzeltmesi: Aynı komut içindeki dummy read'leri filtrele
        if (current_instr == g_last_read_instr) {
            return g_last_read_ret;
        }

        bit = in->pad1_shift & 1;
        in->pad1_shift >>= 1;
        in->pad1_shift |= 0x80; // NES shift register boşalınca 1 döndürür
    }
    
    g_last_read_instr = current_instr;
    g_last_read_ret = (bit & 0x01);
    
    return g_last_read_ret;
}

uint8_t nes_io_read_4017(nes_input_t *in)
{
    return 0x00; 
}