#ifndef EXP_CONFIG_H
#define EXP_CONFIG_H

/*
 * naomines ayarlari. Tek anlamli, az sayida anahtar. Degeri degistirip "make" yapmak yeterli (veya -D ile verin).
 */

/* Acilista sprite modu. 1: HIZLI (varsayilan): oynanista ~2-4 ms daha hizli; sprite tasma bayragi / $2004 okuma / OAM
 * bozulmasi gibi ince donanim davranislari ve fetch penceresi icindeki CHR degisimi taklit edilmez. 0: DOGRULUK
 * (AccuracyCoin 146/146, blargg testleri); agir oyunlarda (ornek Castlevania) 30 fps'i tutamayabilir. Calisirken B3 dugmesi ile
 * degistirilir (EXP_B3_HOLD_MS basili tutarak). Testler icin B3 ile DOGRULUK moduna gecin (sol ustte "DOGRU"). */
#ifndef EXP_FAST_PPU
#define EXP_FAST_PPU 1
#endif

/* Oyun calisirken START'i bu kadar milisaniye basili tutmak ana menuye doner (0: kapali). Uyari gostermez; tus daha
 * once basili olsa bile sayilir. NES oyunu Start'i kendi islevi icin de gorur (ornek: duraklatma): kisa basis normaldir. */
#ifndef EXP_MENU_HOLD_MS
#define EXP_MENU_HOLD_MS 3000
#endif
/* B3 (PPU modu: HIZLI <-> DOGRULUK) bu kadar milisaniye basili tutulunca degisir (yanlislikla basmaya karsi). Her oyun
 * EXP_FAST_PPU moduyla baslar. DOGRULUK modunda sol ustte kirmizi "DOGRU" yazar. */
#ifndef EXP_B3_HOLD_MS
#define EXP_B3_HOLD_MS 1000
#endif

/* 1: menuden secilen oyun B4'e basmadan hemen calisir (B4 duraklatma/surdurme olarak kalir). 0: eski davranis. */
/* OTOMATIK OLCUM (BENCH): 1 = acilista main.c'deki oyun listesini sirayla, girdisiz ve soguk baslangictan calistirir (emulasyon
 * belirlenimci: her derlemede ayni sahneler), sonunda tek ekranlik sonuc tablosu gosterir; START ile normal menuye gecilir.
 * EXP_BENCH_WARM/EXP_BENCH_LOOPS: oyun basina isinma ve olcum dongusu sayisi (dongu = 1 + EXP_BENCH_SKIP NES karesi). */
#ifndef EXP_BENCH
#define EXP_BENCH 1
#endif
#ifndef EXP_BENCH_WARM
#define EXP_BENCH_WARM 300
#endif
#ifndef EXP_BENCH_LOOPS
#define EXP_BENCH_LOOPS 360
#endif
/* 1 = main.c'deki oyun basi tus betigi (oynanisa girmek icin START/A, yurume); 0 = girdisiz (yalniz acilis sahneleri). */
#ifndef EXP_BENCH_INPUT
#define EXP_BENCH_INPUT 1
#endif
/* 1 = (yalniz EXP_BENCH ile) olcum dongulerinde CPU komut adreslerinin histogrami; sonuc ekraninda her oyun icin en sik
 * 5 komut adresi (yuzde + komut baytlari) yazilir. Sicak donguleri (bekleme donguleri vb.) gormek icin TANI amaclidir:
 * hizli yola komut basina bir sayac ekler, bu yuzden bu kipte olculen sureler normalden biraz yuksektir. */
#ifndef EXP_BENCH_PC
#define EXP_BENCH_PC 0
#endif

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
#define EXP_TURBO_FRAMES 1
#endif

/* KARE ATLAMA: gizli karede satir ciziciler PIKSEL URETMEZ (CPU'nun gorebildigi hicbir durum degismez: sprite-0 carpismasi
 * dahil), yalniz dongunun son karesi ekrana cizilir. Tum karelerin SESI korunur.
 *   EXP_AUTO_SKIP 1 (onerilen): UYARLANIR kare atlama + hiz sabitleme. Her dongude, bir onceki dongunun kac vblank surdugu
 *     kadar NES karesi calistirilir (en az 1, en fazla 1 + EXP_FRAME_SKIP). Dongu bir vblank'e sigiyorsa HER KARE cizilir
 *     (60 Hz), sigmiyorsa yalniz gereken kadar kare gizlenir. Oyun hizi hafif sahnelerde de artmaz (eskiden sabit atlamada
 *     iki kat hizlanabiliyordu). EXP_FRAME_SKIP = dongu basina en fazla gizli kare (oyun bundan da agirsa yavaslar).
 *   EXP_AUTO_SKIP 0: sabit kare atlama, dongu basina (1 + EXP_FRAME_SKIP) kare (eski davranis).
 * EXP_FRAME_SKIP 0 ise gizli kare destegi derlenmez (her kare cizilir, otomatik atlama da yapilmaz). */
#ifndef EXP_FRAME_SKIP
#define EXP_FRAME_SKIP 3
#endif
#ifndef EXP_AUTO_SKIP
#define EXP_AUTO_SKIP 1
#endif
/* EXP_AUTO_SKIP: sonraki dongunun kare sayisi = bu dongunun surdugu vblank sayisi (kayipsiz; oyun ve ses hizi dogru). Ek olarak,
 * tahmini is n-1 kareyle (n-1) vblank'e bu payla (us) sigiyorsa bir "deneme" dongusunde kare sayisi bir azaltilir (bir kare
 * kaybedilir); basarisiz denemeden sonra deneme araligi iki katina cikar. */
#ifndef EXP_SKIP_MARGIN_DOWN
#define EXP_SKIP_MARGIN_DOWN 1200
#endif
/* Bench (EXP_BENCH) dongusundeki SABIT gizli kare sayisi (dongu = 1 + EXP_BENCH_SKIP kare). Olcumlerin onceki turlarla
 * karsilastirilabilmesi icin 1. 0'dan buyukse EXP_FRAME_SKIP de 0'dan buyuk olmali (gizli kare destegi). */
#ifndef EXP_BENCH_SKIP
#define EXP_BENCH_SKIP 1
#endif
#if EXP_BENCH && EXP_BENCH_SKIP > 0 && EXP_FRAME_SKIP == 0
#error "EXP_BENCH_SKIP > 0 icin EXP_FRAME_SKIP > 0 gerekli (gizli kare destegi)"
#endif

/* SES: halka tampon boyu (ornek; 4'e bolunebilir, < 65535) ve hedef doluluk (ornek). Hedef ~ ses gecikmesi
 * (2600 / 44100 = ~59 ms; dongu basindaki en dusuk doluluktur). EXP_AUDIO_RATECTL 1: tampon doluluguna gore ornekler en fazla +-%1 yeniden orneklenir
 * (duyulmaz); boylece NES kare hizi (60,10 Hz) ile ekran yenilemesi arasindaki fark ve dongu titresimi tamponu ne
 * bosaltir (takilma) ne tasirir. 0: ornekler oldugu gibi yazilir (eski davranis). */
/* 1 = arka plan rengi libnaomi'ye KAYDEDILMEZ: her vblank'teki 600 KB tam ekran doldurma ve tampon degisimi sonrasi yeniden
 * cizim kalkar. Flycast'te bu kipte oyun ekrani guncellenmiyor (iki denemede de); 0 = libnaomi doldurur (onerilen). */
#ifndef EXP_BG_MANUAL
#define EXP_BG_MANUAL 0
#endif
/* 1 = oyun ekrani TA (Tile Accelerator) ile cizilir: NES karesi 256x256 dokuya kopyalanir (yaklasik eski 1x cizim kadar),
 * 2 kat buyutme ve arka plan temizligi PVR'da yapilir; libnaomi'nin tam ekran doldurmasi ve tampon degisimi sonrasi
 * yeniden cizim gerekmez (EXP_BG_MANUAL/EXP_DRAW_EARLY oyun ekraninda kullanilmaz). Sorun cikarsa 0 (islemciyle 2x cizim). */
#ifndef EXP_TA_DRAW
#define EXP_TA_DRAW 1
#endif
/* 1 (EXP_TA_DRAW ile): PVR cizimi (ta_render) ayri bir libnaomi thread'inde beklenir; ana dongu cizim bitmesini beklemeden
 * sonraki karelerin emulasyonuna gecer (Flycast olcumu: ta_render beklemesi ~4.4 ms). Kare tampon degisiminden hemen sonra
 * baslatilir, bir sonraki degisimde gosterilir: ekrandaki goruntu bir dongu geriden gelir. 0: ta_render cizim bitene kadar bekler. */
#ifndef EXP_TA_ASYNC
#define EXP_TA_ASYNC 1
#endif
/* 1 (EXP_BG_MANUAL 0 iken): NES karesi dongu basina YALNIZ BIR KEZ, tampon degisiminden hemen sonra cizilir (2x cizim
 * maliyeti yarilanir; ekrandaki kare bir dongu, yani 1-3 NES karesi kadar geriden gelir). 0: iki cizim (eski). */
#ifndef EXP_DRAW_EARLY
#define EXP_DRAW_EARLY 1
#endif
/* 2x goruntu cizimi: 0 = islemci (32 bit yazim), 1 = libnaomi hw_memcpy (store queue; Flycast'te yavas olculdu). */
#ifndef EXP_BLIT_SQ
#define EXP_BLIT_SQ 0
#endif
/* 1 = oyun sirasinda sol ustteki fps'e ek olarak iki tani satiri (ms dokumu, kare/dongu, ses tamponu). */
#ifndef EXP_HUD_DETAIL
#define EXP_HUD_DETAIL 0
#endif
/* 1 = fps'in altinda dongu basina ortalama NES karesi ("x1.0": her kare cizilir, 60 Hz; "x2.0": iki karede bir).
 * 2 = ek olarak kare sayisi kararinin tahminleri (ms): F sabit is, d cizilen kare, h gizli kare; a: kare azaltma denemesi,
 * s: basarisiz deneme; ses: o hedef, c kapasite, b bosalma, t tasma, p butun parti atilan. 0 = kapali. */
#ifndef EXP_HUD_NFR
#define EXP_HUD_NFR 0
#endif

#ifndef EXP_AUDIO_RING
#define EXP_AUDIO_RING 8192
#endif
#ifndef EXP_AUDIO_TARGET
#define EXP_AUDIO_TARGET 2600
#endif
#ifndef EXP_AUDIO_RATECTL
#define EXP_AUDIO_RATECTL 1
#endif
/* 1 = tampon dolulugu libnaomi'nin ic AICA komutuyla (okuma konumu) OLCULUR (saat kaymasina dayanikli); 0 = yalniz sureden
 * tahmin edilir. Derleme "__audio_exchange_command" bulunamadi hatasi verirse 0 yapin. */
#ifndef EXP_AUDIO_HWPOS
#define EXP_AUDIO_HWPOS 1
#endif
/* 1 (EXP_AUDIO_HWPOS ile) = ses orneklerini libnaomi'nin halka tamponuna DOGRUDAN yaz (adresler libnaomi'den bir kez alinir):
 * libnaomi yaziminin cagri basina 2 konum sorgusu ve ornek basina modulo bolmesi kalkar (Flycast'te yazim 1,1-1,8 ms olculdu).
 * Ses bozulursa 0 (libnaomi audio_write_stereo_data). */
#ifndef EXP_AUDIO_DIRECT
#define EXP_AUDIO_DIRECT 1
#endif
/* APU SES CIKISI (CPU'ya gorunmez; AccuracyCoin'i etkilemez):
 *   EXP_APU_DMC_SILENCE 1: DMC sessizlik bayragi (ornek tamponu bosken cikis seviyesi degismez; donanim). 0: eski davranis
 *     (seviye her bitte 2 azalir; $4011'e her karede yazan oyunlarda, ornek Super Mario Bros, 60 Hz ugultu).
 *   EXP_APU_TRI_HOLD 1: susturulan ucgen kanali son degerini verir (donanim). 0: eski davranis (0'a zorlanir; nota
 *     sonlarinda tik). */
#ifndef EXP_APU_DMC_SILENCE
#define EXP_APU_DMC_SILENCE 1
#endif
#ifndef EXP_APU_TRI_HOLD
#define EXP_APU_TRI_HOLD 1
#endif

#if EXP_FRAME_SKIP > 0 && EXP_TURBO_FRAMES > 1
#error "EXP_FRAME_SKIP ile EXP_TURBO_FRAMES > 1 birlikte kullanilamaz"
#endif

/* $2002 YOKLAMA ATLAMASI (varsayilan 1, BIT-EXACT tasarlandi): her turda yalniz $2002 okuyan ve ayni degeri goren bekleme
 * donguleri (sprite-0 carpismasi / tasma / VBL bekleme: "BIT $2002 / BVC geri" gibi), PPU durum bitlerinin DEGISEMEYECEGI
 * kanitlanan araliga kadar tek hamlede atlanir (PPU adimlanmaz, tembel kalir). Bos dongu atlamasinin (ff:) uzantisidir;
 * atlanan cycle ff: degerine eklenir. 0 = kapali (onceki davranis). */
#ifndef EXP_POLL_FF
#define EXP_POLL_FF 1
#endif

/* PRG BANK ISARETCILERI: 1 = bank degisimi 8 KB pencereyi kopyalamaz, pencere isaretcisini degistirir (her PRG okumasinda bir ek
 * yukleme; bank degistiren oyunlarda kopya yuku kalkar). 0 = eski kopyalama (bus->mem[$8000-$FFFF] her zaman guncel). Davranis ayni. */
/* CPU HIZLI YOL DAGITIMI: 1 = opcode basina tek case (daha az komut/dagitim), 0 = eski iki asamali (mod + islem). Davranis ayni. */
/* 1 = hizli yolda "DEY|DEX ; BNE *-1" gecikme donguleri kapali formda atlanir (sonuc birebir ayni; bkz. fake6502_fast.inc). */
#ifndef EXP_DELAY_FF
#define EXP_DELAY_FF 1
#endif
/* 1 = hizli yolda yalniz RAM'e "INC" yapan ve kendine JMP ile donen sayac bekleme donguleri kapali formda atlanir
 * (sonuc birebir ayni; bkz. fake6502_fast.inc, JMP). */
#ifndef EXP_INCLOOP_FF
#define EXP_INCLOOP_FF 1
#endif
#ifndef EXP_FAST_DISPATCH
#define EXP_FAST_DISPATCH 1
#endif

#ifndef EXP_PRG_PTR
#define EXP_PRG_PTR 1
#endif

/* KARE HIZALAMA: dongu siniri NES karesinin VBL bolgesine (PPU satiri EXP_ALIGN_LINE) kaydirilir. Etkisi: ekranda yirtilma
 * olmaz (her zaman tam bir kare) ve kare atlamada gizli kare 240 satirin tamamini atlar (hizalamasiz bazi satirlari gorunur
 * turda cizilir). Emulasyon degismez (yalniz dongu sinirlari kayar). Varsayilan: EXP_FRAME_SKIP > 0 ise acik. */
#ifndef EXP_FRAME_ALIGN
#define EXP_FRAME_ALIGN (EXP_FRAME_SKIP > 0)
#endif
#ifndef EXP_ALIGN_LINE
#define EXP_ALIGN_LINE 250
#endif

/* OLCUM KIPI (varsayilan 0; yalniz olcum icin): emulasyon suresini PPU / APU / CPU+surucu olarak boler. Dongu dongu sirayla:
 *   kip 0: normal;  kip 1: "golge PPU" (PPU'nun bir kopyasi ayni cagrilarla ikinci kez, piksel uretmeden adimlanir);
 *   kip 2: "golge APU" (APU'nun bir kopyasi ayni cagrilarla ikinci kez adimlanir).
 * Kip 1 - kip 0 = PPU adimlama maliyeti (piksel haric), kip 2 - kip 0 = APU maliyeti. Gercek emulasyon DEGISMEZ (golge
 * kopyalarin mapper saati / IRQ / ekran yan etkileri bastirilir); yalniz golge kiplerinde fps duser. EXP_DIAG_HUD'u acar.
 * Dorduncu tani satiri: bkz. main.c. */
#ifndef EXP_DIAG_PROF
#define EXP_DIAG_PROF 1
#endif
#if EXP_DIAG_PROF
#undef EXP_DIAG_HUD
#define EXP_DIAG_HUD 1
#endif

/* TANI SATIRI (varsayilan 0): 1 = fps satirinin altina ikinci bir satir cizilir (~0,5 ms metin maliyeti):
 *   atla:N   EXP_FRAME_SKIP degeri
 *   gizli:N  son dongude piksel uretimi ATLANAN satir sayisi (kare atlama calisiyorsa gizli kare basina ~240)
 *   dw:N     son dongude nokta-nokta (yavas) yoldan cizilen satir sayisi (satir ortasi yazmac degisimi olan satirlar)
 *   s2002:N  son dongude $2002 (PPU durum) okuma sayisi (sprite-0 / vblank yoklama donguleri)
 *   pf:N     son dongude $2002 yoklama atlamasiyla (EXP_POLL_FF) gecilen CPU cycle (ff: icinde de sayilir)
 *   io:N     diger PPU/APU/kartus yazmac erisimleri
 *   g: / c:  gizli kare ve cizilen kare icin ayri emulasyon suresi (ms)
 * Ucuncu satir (yoklama atlamasi tanisi, son dongu): "yok" ret sayaclari
 *   d: dongu turu farkli yazmac/PC ile bitti   e: turda $2002 disinda yavas erisim (RAM yazimi, baska G/C) veya 1'den fazla okuma
 *   b: okunan degerde bit 7   k: onceki turla uzunluk/okuma ani/deger farkli   a: son tarih/kare siniri cok yakin
 *   p: PPU'nun anlik durumu okunan degerden farkli   z: kararli aralik bir tura yetmiyor   ok: atlandi
 *   R: son $2002 okumasini yapan komutun adresi ve ilk 6 bayti   B: son yoklama turunu kapatan dallanmanin adresi */
#ifndef EXP_DIAG_HUD
#define EXP_DIAG_HUD 0
#endif

/* ppu.c icinde sabit kalan kullanimlar (olcum/ablasyon altyapisi kaldirildi). */
#define EXP_SPR_PIPE_ON    1
#define EXP_LINE_RENDER_ON 1

#endif