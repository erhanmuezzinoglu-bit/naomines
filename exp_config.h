#ifndef EXP_CONFIG_H
#define EXP_CONFIG_H

/*
 * Deney anahtarlari (v2). Degeri degistirip "make" yapmak yeterli.
 * Her anahtar 0 yapildiginda ilgili kod ORIJINAL davranisa doner.
 */

/* Zamanlama olcumu + HUD'a olcum satirlari. */
#ifndef EXP_PROFILE
#define EXP_PROFILE 1
#endif

/* Dongudeki video_fill_screen() cagrisini kaldir. */
#ifndef EXP_NO_FILL
#define EXP_NO_FILL 1
#endif

/* ppu.c: donanim formatinda uint16_t screen_buffer + palet LUT + satir blit. */
#ifndef EXP_FAST_BLIT
#define EXP_FAST_BLIT 1
#endif

/* Bir dongude kac emule frame (sadece sonuncusu cizilir, ara frame sesi atilir).
 * NOT: emulasyon baskin oldugu icin turbo bir fayda saglamaz; 1 birak. */
#ifndef EXP_TURBO_FRAMES
#define EXP_TURBO_FRAMES 1
#endif

/* Acilista bir kez CPU/cache kalibrasyon testi (~0.3 sn) kosar, sonucu HUD'a yazar. */
#ifndef EXP_BENCH
#define EXP_BENCH 1
#endif

/* 1: PPU her CPU cycle'inda degil, yalniz "disariya gorunur" olaylarda (VBL/NMI, mapper A12
 *    saati), PPU/kartus yazmac erisimlerinde ve komut sonlarinda senkronlanir (BIT-EXACT
 *    tasarlandi; AccuracyCoin/blargg ile dogrula). 0: eski cycle-basi senkron. */
#ifndef EXP_LAZY_PPU
#define EXP_LAZY_PPU 1
#endif

/* 1: APU her CPU cycle'inda degil, yalniz CPU'ya gorunen olaylarda (DMC sayac reload'i,
 *    frame-sequencer olaylari), APU yazmac erisimlerinde ve ses bosaltmadan once
 *    senkronlanir; olay icermeyen cycle'lar kapali formda toplu ilerletilir (BIT-EXACT
 *    tasarlandi; AccuracyCoin/blargg ile dogrula). 0: eski cycle-basi senkron. */
#ifndef EXP_LAZY_APU
#define EXP_LAZY_APU 1
#endif

/* SADECE OLCUM: emulasyon icinde zamanin nereye gittigini bulmak icin bilesenleri
 * sirayla devre disi birakir (autorun acikken kendiliginden modlar arasinda gezer).
 * Bu modlarda emulasyon DOGRU DEGILDIR (goruntu/ses bozulur). Olcum bitince 0 yap.
 *   M0: normal
 *   M1: APU adimi kapali                                  -> APU payi      = M0-M1
 *   M2: PPU sprite pipeline kapali                        -> sprite payi   = M0-M2
 *   M3: PPU satir cizicisi (render_scanline) kapali       -> cizici payi   = M0-M3
 *   M4: M1+M2+M3 (APU + sprite + cizici kapali)           -> kalan PPU     = M4-M5
 *   M5: M4 + PPU adimlama tamamen kapali (CPU+bus+surucu; oyun vblank'ta bekler)
 *   M6: M5 + nes_bus_read/write dagitimi atlanir (RAM/PRG dogrudan)
 *   M7: M6 + surucu sarmalayicisi (DMC/PPU/APU sync, IRQ snapshot) atlanir = CPU cekirdegi
 */
#ifndef EXP_ABLATE
#define EXP_ABLATE 0
#endif

#if EXP_ABLATE && !EXP_PROFILE
#error "EXP_ABLATE icin EXP_PROFILE=1 gerekli"
#endif

#define EXP_NMODES 8

#if EXP_ABLATE
extern int g_exp_mode;
#define EXP_SPR_PIPE_ON   (g_exp_mode != 2 && g_exp_mode != 4)
#define EXP_LINE_RENDER_ON (g_exp_mode != 3 && g_exp_mode != 4)
#else
#define EXP_SPR_PIPE_ON   1
#define EXP_LINE_RENDER_ON 1
#endif

#endif