#include "nes_rom.h"
#include "exp_config.h"
#include "nes_bus.h"
#include "mapper.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- Global değişkenler ---------------- */
uint8_t *nes_prg_banks[NES_MAX_PRG_BANKS] = {0};
/* CPU $8000-$FFFF gorunumu.
 * nes_prg_page[w]: 8 KB pencere w'nin ($8000 + w*$2000) kaynak isaretcisi (mapper.c set_prg_8k degistirir).
 * nes_prg_img: 64 KB adres duzenli tampon; $8000-$FFFF kismi bu dort pencereye ESIT (alt yarisi kullanilmaz). CPU okumalari
 * mem[] ile ayni indeksleme: nes_prg_img[adres] (adres >= $8000).
 * EXP_PRG_PTR 1: goruntuler, pencere dortlusune gore anahtarlanan kucuk bir onbellekte tutulur (nes_prg_commit). Ayni dortluye geri
 * donen bank degisimi kopyasizdir; yeni dortlu en eski goruntuyu yalniz FARKLI pencereleri kopyalayarak yeniden kullanir.
 * EXP_PRG_PTR 0: eski davranis (pencereler mem[]'e kopyalanir, goruntu mem[]). */
static const uint8_t nes_prg_zero_page[0x10000];
const uint8_t *nes_prg_page[4] = { nes_prg_zero_page, nes_prg_zero_page, nes_prg_zero_page, nes_prg_zero_page };
const uint8_t *nes_prg_img = nes_prg_zero_page;
int nes_prg_dirty = 0;
unsigned long nes_prg_copies = 0;   /* tani: goruntuye yapilan 8 KB pencere kopyasi sayisi */
#if EXP_PRG_PTR
#define PRG_IMG_N 8
static uint8_t prg_img[PRG_IMG_N][0x10000];   /* yalniz $8000-$FFFF yarisi kullanilir */
static const uint8_t *prg_img_src[PRG_IMG_N][4];
static unsigned prg_img_age[PRG_IMG_N], prg_age_now;
static int prg_img_ok[PRG_IMG_N];
static int prg_cur = -1;

static void nes_prg_cache_clear(void)
{
    for (int i = 0; i < PRG_IMG_N; i++) prg_img_ok[i] = 0;
    prg_cur = -1;
}

void nes_prg_commit(void)
{
    nes_prg_dirty = 0;
    if (prg_cur >= 0 && prg_img_src[prg_cur][0] == nes_prg_page[0] && prg_img_src[prg_cur][1] == nes_prg_page[1] &&
        prg_img_src[prg_cur][2] == nes_prg_page[2] && prg_img_src[prg_cur][3] == nes_prg_page[3]) return;
    int v = -1;
    for (int i = 0; i < PRG_IMG_N; i++) {
        if (prg_img_ok[i] && prg_img_src[i][0] == nes_prg_page[0] && prg_img_src[i][1] == nes_prg_page[1] &&
            prg_img_src[i][2] == nes_prg_page[2] && prg_img_src[i][3] == nes_prg_page[3]) { v = i; break; }
    }
    if (v < 0) {
        /* yeni dortlu: bos ya da en eski goruntu; yalniz farkli pencereleri kopyala */
        unsigned best = 0;
        for (int i = 0; i < PRG_IMG_N; i++) {
            if (!prg_img_ok[i]) { v = i; break; }
            const unsigned a = prg_age_now - prg_img_age[i];
            if (v < 0 || a > best) { v = i; best = a; }
        }
        for (int w = 0; w < 4; w++) {
            if (!prg_img_ok[v] || prg_img_src[v][w] != nes_prg_page[w]) {
                memcpy(&prg_img[v][0x8000 + w * 0x2000], nes_prg_page[w], 0x2000);
                prg_img_src[v][w] = nes_prg_page[w];
                nes_prg_copies++;
            }
        }
        prg_img_ok[v] = 1;
    }
    prg_img_age[v] = ++prg_age_now;
    prg_cur = v;
    nes_prg_img = prg_img[v];
}
#else
void nes_prg_commit(void) { nes_prg_dirty = 0; }
#endif
int nes_prg_banks_count = 0;

uint8_t *nes_chr_banks[NES_MAX_CHR_BANKS] = {0};
int nes_chr_banks_count = 0;

int nes_last_mapper_loaded = 0;
int nes_chr_is_rom = 0;

mapper_state_t g_mapper;

static uint8_t *last_prg_heap = NULL;
static uint8_t *last_chr_heap = NULL;

static int ines_valid(const uint8_t hdr[16]) {
    return (hdr[0] == 'N' && hdr[1] == 'E' && hdr[2] == 'S' && hdr[3] == 0x1A);
}

static uint16_t ines_mapper(const uint8_t hdr[16]) {
    uint16_t mapper = (uint16_t)((hdr[7] & 0xF0) | ((hdr[6] & 0xF0) >> 4));
    if ((hdr[7] & 0x0C) == 0x08) {
        mapper |= (uint16_t)((hdr[8] & 0x0F) << 8);
    }
    return mapper;
}

/* PRG bankları 8KB granülarite ile yükle, mem_64k'ya doğru power-on mapping'i kopyala. */
static void load_prg(const uint8_t *data, int prg_bytes,
                     uint16_t mapper, uint8_t mem_64k[0x10000])
{
    int banks_8k = prg_bytes / 8192;
    if (banks_8k > NES_MAX_PRG_BANKS) banks_8k = NES_MAX_PRG_BANKS;

    for (int i = 0; i < banks_8k; i++) {
        nes_prg_banks[i] = (uint8_t *)data + i * 8192;
    }
    nes_prg_banks_count = banks_8k;

    uint8_t *src_8000 = NULL;
    uint8_t *src_A000 = NULL;
    uint8_t *src_C000 = NULL;
    uint8_t *src_E000 = NULL;

    switch (mapper) {
        case 0: /* NROM */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[2];
                src_E000 = nes_prg_banks[3];
            } else if (banks_8k == 2) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[0];
                src_E000 = nes_prg_banks[1];
            } else if (banks_8k == 1) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[0];
                src_C000 = nes_prg_banks[0];
                src_E000 = nes_prg_banks[0];
            }
            break;

        case 1: /* MMC1 */
            if (banks_8k >= 2) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        case 2: /* UNROM */
            if (banks_8k >= 2) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        case 3: /* CNROM */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[2];
                src_E000 = nes_prg_banks[3];
            } else if (banks_8k == 2) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[0];
                src_E000 = nes_prg_banks[1];
            }
            break;

        case 4: /* MMC3 */
    if (banks_8k >= 4) {
        src_8000 = nes_prg_banks[0];
        src_A000 = nes_prg_banks[1];
        src_C000 = nes_prg_banks[banks_8k - 2];
        src_E000 = nes_prg_banks[banks_8k - 1];
    } else if (banks_8k == 2) {
        /* 16KB MMC3 test ROM'ları: PRG'yi $8000-$FFFF'e mirror et. */
        src_8000 = nes_prg_banks[0];
        src_A000 = nes_prg_banks[1];
        src_C000 = nes_prg_banks[0];
        src_E000 = nes_prg_banks[1];
    }
    break;

        case 7: /* AxROM */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[2];
                src_E000 = nes_prg_banks[3];
            }
            break;

        case 9: /* MMC2: 8KB switchable + last 24KB fixed */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[banks_8k - 3];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        case 28: /* Action 53 (INL-ROM): power-on = SON 32K aktif.
                  * Reset vektörü son bankta bulunur.
                  * a53_apply() daha sonra register durumuna göre
                  * yeniden map eder. */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[banks_8k - 4];
                src_A000 = nes_prg_banks[banks_8k - 3];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        case 78: /* Holy Diver (IF-12): 16K switchable + 16K fixed last */
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        case 180: /* UNROM Crazy (7408): same PRG banking as M2 */
            if (banks_8k >= 2) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[banks_8k - 2];
                src_E000 = nes_prg_banks[banks_8k - 1];
            }
            break;

        default:
            if (banks_8k >= 4) {
                src_8000 = nes_prg_banks[0];
                src_A000 = nes_prg_banks[1];
                src_C000 = nes_prg_banks[2];
                src_E000 = nes_prg_banks[3];
            }
            break;
    }

    /* Pencere icerigi eskiden mem_64k'ya kopyalanirdi; kaynak yoksa (garip boyutlar) pencere mem_64k'nin onceki icerigini gosterirdi.
     * Ayni davranis: kopya korunur, pencere isaretcisi kaynagi (yoksa mem_64k'daki eski icerigi) gosterir. */
    if (src_8000) memcpy(&mem_64k[0x8000], src_8000, 8192);
    if (src_A000) memcpy(&mem_64k[0xA000], src_A000, 8192);
    if (src_C000) memcpy(&mem_64k[0xC000], src_C000, 8192);
    if (src_E000) memcpy(&mem_64k[0xE000], src_E000, 8192);
#if EXP_PRG_PTR
    nes_prg_page[0] = src_8000 ? src_8000 : &mem_64k[0x8000];
    nes_prg_page[1] = src_A000 ? src_A000 : &mem_64k[0xA000];
    nes_prg_page[2] = src_C000 ? src_C000 : &mem_64k[0xC000];
    nes_prg_page[3] = src_E000 ? src_E000 : &mem_64k[0xE000];
    nes_prg_cache_clear();   /* yeni ROM: eski goruntuler (ayni heap adresleri olabilir) gecersiz */
    nes_prg_commit();
#else
    /* kopyalama kipi: pencereler ve goruntu her zaman mem_64k'yi gosterir */
    for (int w_ = 0; w_ < 4; w_++) nes_prg_page[w_] = &mem_64k[0x8000 + 0x2000 * w_];
    nes_prg_img = mem_64k;
#endif
}

int nes_rom_load_mapper0(const char *rompath, uint8_t mem_64k[0x10000], ppu_t *ppu)
{
    FILE *fp = fopen(rompath, "rb");
    if (!fp) return 0;

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, fp) != 16 || !ines_valid(hdr)) {
        fclose(fp);
        return 0;
    }

    uint8_t prg_banks_16k = hdr[4];
    uint8_t chr_banks_8k = hdr[5];
    uint8_t flags6 = hdr[6];
    uint16_t mapper = ines_mapper(hdr);
    nes_last_mapper_loaded = mapper;

    /* Tüm mapper state'i, static IRQ state'i dahil, power-on durumuna getir. */
    mapper_reset_state();

    if (mapper == 4) {
        /* Header mirroring'i power-on görüntüsü için koru; $A000 sonradan override eder. */
        g_mapper.mmc3_mirroring = (flags6 & 0x01) ? 0 : 1;
    }

    if (ppu) {
        ppu->mirroring = (flags6 & 0x01) ? 1 : 0;
    }

    long prg_off = 16 + ((flags6 & 0x04) ? 512 : 0);
    fseek(fp, prg_off, SEEK_SET);

    if (last_prg_heap) { free(last_prg_heap); last_prg_heap = NULL; }
    if (last_chr_heap) { free(last_chr_heap); last_chr_heap = NULL; }

    for (int i = 0; i < NES_MAX_PRG_BANKS; i++) nes_prg_banks[i] = NULL;
    for (int i = 0; i < NES_MAX_CHR_BANKS; i++) nes_chr_banks[i] = NULL;
    nes_prg_banks_count = 0;
    nes_chr_banks_count = 0;

    /* ---------------- PRG yükle ---------------- */
    uint32_t prg_len = (uint32_t)prg_banks_16k * 16384;
    if (prg_len == 0) {
        fclose(fp);
        return 0;
    }

    last_prg_heap = (uint8_t *)malloc(prg_len);
    if (!last_prg_heap) { fclose(fp); return 0; }

    if (fread(last_prg_heap, 1, prg_len, fp) != prg_len) {
        fclose(fp);
        free(last_prg_heap);
        last_prg_heap = NULL;
        return 0;
    }

    load_prg(last_prg_heap, (int)prg_len, mapper, mem_64k);

    /* ---------------- CHR yükle ---------------- */
    if (ppu) {
        if (chr_banks_8k > 0) {
            nes_chr_is_rom = 1;

            uint32_t chr_len = (uint32_t)chr_banks_8k * 8192;
            last_chr_heap = (uint8_t *)malloc(chr_len);
            if (last_chr_heap) {
                fseek(fp, prg_off + prg_len, SEEK_SET);
                if (fread(last_chr_heap, 1, chr_len, fp) != chr_len) {
                    memset(last_chr_heap, 0, chr_len);
                }

                int chr_banks_1k = chr_len / 1024;
                if (chr_banks_1k > NES_MAX_CHR_BANKS)
                    chr_banks_1k = NES_MAX_CHR_BANKS;

                for (int i = 0; i < chr_banks_1k; i++) {
                    nes_chr_banks[i] = last_chr_heap + i * 1024;
                }
                nes_chr_banks_count = chr_banks_1k;

                /* İlk 8KB'ı PPU vram'e yükle. */
                if (mapper == 9) {
                    /* MMC2 power-on: latch'ler FE, ilgili FE bankleri başlangıçta 0. */
                    memcpy(&ppu->vram[0x0000], last_chr_heap, 4096);
                    memcpy(&ppu->vram[0x1000], last_chr_heap, 4096);
                    mapper_mmc2_setup();
                } else {
                    memcpy(ppu->vram, last_chr_heap, 8192);
                }
            }
        } else {
            /* CHR RAM — yazılabilir, 8 tane 1KB pointer ile vram'e işaret et */
            nes_chr_is_rom = 0;
            memset(ppu->vram, 0, 8192);
            for (int i = 0; i < 8; i++) {
                nes_chr_banks[i] = ppu->vram + i * 1024;
            }
            nes_chr_banks_count = 8;
        }
    }

    fclose(fp);
    printf("ROM Yuklendi: Mapper %u, PRG 16K:%d, CHR 8K:%d, CHR_IS_ROM=%d\n",
           mapper, prg_banks_16k, chr_banks_8k, nes_chr_is_rom);
    mapper_refresh_ppu_dep();   /* CHR turu/sayisi ve mapper artik belli */
    return 1;
}