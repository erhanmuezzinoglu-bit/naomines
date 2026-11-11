#ifndef EXP_CONFIG_H
#define EXP_CONFIG_H

/*
 * naomines ayarlari. Tek anlamli, az sayida anahtar. Degeri degistirip "make" yapmak yeterli (veya -D ile verin).
 */

/* Acilista sprite modu. 1: HIZLI (varsayilan): oynanista ~2-4 ms daha hizli; sprite tasma bayragi / $2004 okuma / OAM
 * bozulmasi gibi ince donanim davranislari ve fetch penceresi icindeki CHR degisimi taklit edilmez. 0: DOGRULUK
 * (AccuracyCoin 146/146, blargg testleri); agir oyunlarda (ornek Castlevania) 30 fps'i tutamayabilir. Calisirken B3 dugmesi ile
 * degistirilir. Testler icin B3 ile "ppu:dogru" yapin. */
#ifndef EXP_FAST_PPU
#define EXP_FAST_PPU 1
#endif

/* Oyun calisirken START'i bu kadar milisaniye basili tutmak ana menuye doner (0: kapali). Uyari gostermez; tus daha
 * once basili olsa bile sayilir. NES oyunu Start'i kendi islevi icin de gorur (ornek: duraklatma): kisa basis normaldir. */
#ifndef EXP_MENU_HOLD_MS
#define EXP_MENU_HOLD_MS 3000
#endif

/* 1: menuden secilen oyun B4'e basmadan hemen calisir (B4 duraklatma/surdurme olarak kalir). 0: eski davranis. */
#ifndef EXP_AUTORUN_ON_LOAD
#define EXP_AUTORUN_ON_LOAD 1
#endif

/* Giris (maple/JVS) okuma sikligi: her EXP_INPUT_PERIOD dongu karesinde bir. 1: her karede. 2 secilirse okuma maliyeti
 * (~0.9 ms) yarilanir; kisa (<2 kare) tus vuruslari kacabilir. */
#ifndef EXP_INPUT_PERIOD
#define EXP_INPUT_PERIOD 1
#endif

/* Bir dongude kac emule frame (sadece sonuncusu cizilir). Emulasyon baskin oldugu icin 1 birak. */
#ifndef EXP_TURBO_FRAMES
#define EXP_TURBO_FRAMES 0
#endif

/* KARE ATLAMA (varsayilan 0 = KAPALI): bir dongude (1 + EXP_FRAME_SKIP) NES karesi calistirilir; ilk EXP_FRAME_SKIP kare
 * GIZLIDIR (satir ciziciler piksel uretmez, CPU'nun gorebildigi hicbir durum degismez: sprite-0 carpismasi dahil), yalniz
 * sonuncusu ekrana cizilir. Tum karelerin SESI korunur. Sonuc: oyun HIZI artar (hafif oyunlarda tam hiz, 60 NES kare/sn),
 * ekrana basilan kare sayisi azalir (1 icin en fazla 30 Hz; oyun agirsa 20 Hz'e inebilir). 1 onerilir.
 * (2 ve uzeri: dongu basina 3+ kare = 2205+ ses ornegi; ses halkasi 2048 ornek oldugundan yazma bloklar, gecikme artar.) */
#ifndef EXP_FRAME_SKIP
#define EXP_FRAME_SKIP 1
#endif

#if EXP_FRAME_SKIP > 0 && EXP_TURBO_FRAMES > 1
#error "EXP_FRAME_SKIP ile EXP_TURBO_FRAMES > 1 birlikte kullanilamaz"
#endif

/* ppu.c icinde sabit kalan kullanimlar (olcum/ablasyon altyapisi kaldirildi). */
#define EXP_SPR_PIPE_ON    1
#define EXP_LINE_RENDER_ON 1

#endif