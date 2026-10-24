#include "rom_browser.h"

#include <naomi/video.h>
#include <naomi/romfs.h>

#include <dirent.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  ROM Browser GUI — Görsel sürüm (self-contained)
 *
 *  Bağımlılıklar: rom_browser.h + libnaomi (video, romfs)
 *  Yedekler arasında taşımak için yalnızca bu dosyayı kopyala.
 * ============================================================ */

/* ---------------- Sabitler ---------------- */

#define MAX_ROMS        128
#define ROM_NAME_LEN    64
#define SCREEN_W        640
#define SCREEN_H        480

/* ---------------- Tema: renkler ---------------- */

/* Arka plan gradient üç noktası */
#define C_BG_TOP        rgb(6,   8,  22)
#define C_BG_MID        rgb(22, 18,  54)
#define C_BG_BOT        rgb(3,   3,  10)

/* Panel kabartma */
#define C_PANEL_FILL    rgb(12,  14,  30)
#define C_PANEL_FILL2   rgb(18,  22,  46)
#define C_PANEL_DARK    rgb(2,    2,   8)
#define C_PANEL_LIGHT   rgb(70,  80, 140)
#define C_PANEL_EDGE    rgb(110, 130, 210)
#define C_PANEL_SHADOW  rgb(0,    0,   0)

/* Başlık şeridi */
#define C_BAR_BG        rgb(30,  20,  60)
#define C_BAR_BG2       rgb(50,  30,  90)
#define C_BAR_EDGE      rgb(140, 100, 220)

/* Metin */
#define C_TITLE         rgb(255, 210,  90)
#define C_TITLE_SHD     rgb(80,  50,   0)
#define C_TITLE_GLOW    rgb(255, 240, 180)
#define C_SUBTITLE      rgb(150, 170, 220)
#define C_HINT          rgb(120, 200, 255)
#define C_DIM           rgb(90,  90, 130)
#define C_RULE          rgb(70,  80, 140)
#define C_RULE_HI       rgb(140, 160, 240)

/* Liste öğeleri */
#define C_ITEM          rgb(200, 208, 230)
#define C_ITEM_ALT      rgb(24,  26,  48)
#define C_ITEM_NUM      rgb(110, 110, 150)
#define C_ITEM_SEL_FG   rgb(255, 255, 255)
#define C_ITEM_SEL_BG   rgb(40,  80, 180)
#define C_ITEM_SEL_BG2  rgb(20,  45, 120)
#define C_ITEM_SEL_ACC  rgb(255, 200,  80)
#define C_ITEM_SEP      rgb(40,  44,  80)

/* Sağ panel */
#define C_CART_BODY     rgb(60,  50,  30)
#define C_CART_EDGE     rgb(200, 160,  80)
#define C_CART_LABEL    rgb(230, 200, 120)
#define C_CART_STRIPE   rgb(140, 100,  40)
#define C_INFO_KEY      rgb(120, 150, 220)
#define C_INFO_VAL      rgb(220, 220, 240)

/* Alt bilgi */
#define C_FOOTER_BG     rgb(4,   4,  14)
#define C_FOOTER_TXT    rgb(150, 170, 210)

/* Sayaç / progress */
#define C_PROG_BG       rgb(20,  22,  50)
#define C_PROG_FILL_A   rgb(80, 200, 255)
#define C_PROG_FILL_B   rgb(40, 120, 220)
#define C_PROG_EDGE     rgb(120, 180, 255)

/* Yıldızlar */
#define C_STAR_DIM      rgb(40,  44,  70)
#define C_STAR_BRIGHT   rgb(120, 140, 200)

/* ---------------- Yerleşim ---------------- */

#define OUTER_MARGIN    8

#define HEADER_Y        10
#define HEADER_H        66

#define BODY_Y          84
#define BODY_H          316

#define LIST_X          20
#define LIST_W          376

#define INFO_X          (LIST_X + LIST_W + 14)   /* 410 */
#define INFO_W          (SCREEN_W - INFO_X - 20) /* 210 */

#define ITEM_H          26
#define LIST_HEADER_H   24
#define VISIBLE_ITEMS   10

#define COUNTER_Y       (BODY_Y + BODY_H + 8)
#define COUNTER_H       20

#define FOOTER_Y        (SCREEN_H - 40)
#define FOOTER_H        32

/* ---------------- İç state ---------------- */

static char     g_rom_names[MAX_ROMS][ROM_NAME_LEN];
static int      g_rom_count     = 0;
static int      g_scroll_offset = 0;
static uint32_t g_frame         = 0;

/* ---------------- Küçük yardımcılar ---------------- */

static int text_w(const char *s) { return (int)strlen(s) * 8; }

static void text_center(int y, color_t c, const char *s)
{
    video_draw_debug_text((SCREEN_W - text_w(s)) / 2, y, c, "%s", s);
}

static void text_center_shadow(int y, color_t fg, color_t sh, const char *s)
{
    int x = (SCREEN_W - text_w(s)) / 2;
    video_draw_debug_text(x + 2, y + 2, sh, "%s", s);
    video_draw_debug_text(x,     y,     fg, "%s", s);
}

/* Yumuşak üçgen dalga: 0..period arası gidip gelir */
static int pulse(int period)
{
    int p = (int)(g_frame % (uint32_t)(period * 2));
    return (p < period) ? p : (period * 2 - p);
}

/* ---------------- Gradient arka plan ---------------- */

static void draw_background(void)
{
    const int strips = 32;
    for (int i = 0; i < strips; i++) {
        int y0 = i * SCREEN_H / strips;
        int y1 = (i + 1) * SCREEN_H / strips;

        int t = i * 255 / (strips - 1);
        int r, g, b;

        if (t < 128) {
            int u = t * 2;
            /* TOP -> MID */
            r = ( 6 * (255 - u) + 22 * u) / 255;
            g = ( 8 * (255 - u) + 18 * u) / 255;
            b = (22 * (255 - u) + 54 * u) / 255;
        } else {
            int u = (t - 128) * 2;
            /* MID -> BOT */
            r = (22 * (255 - u) +  3 * u) / 255;
            g = (18 * (255 - u) +  3 * u) / 255;
            b = (54 * (255 - u) + 10 * u) / 255;
        }
        video_fill_box(0, y0, SCREEN_W, y1, rgb(r, g, b));
    }
}

/* Deterministik "yıldız alanı" — üst bölgede hafif animasyonlu */
static void draw_starfield(void)
{
    for (int i = 0; i < 90; i++) {
        int x = (i * 137 + 41) % SCREEN_W;
        int y = (i * 73 + 17) % 200;

        /* Çok koyu olduğu için okunabilirliği bozmaz */
        int twinkle = (int)((g_frame / 6 + i * 5) % 40);
        if (twinkle < 4) {
            video_draw_pixel(x, y, C_STAR_BRIGHT);
        } else if (twinkle < 14) {
            video_draw_pixel(x, y, rgb(70, 78, 110));
        } else {
            video_draw_pixel(x, y, C_STAR_DIM);
        }
    }
}

/* İnce yatay "grid" çizgileri — CRT hissi */
static void draw_scan_hints(void)
{
    for (int y = 4; y < SCREEN_H; y += 32) {
        video_fill_box(0, y, SCREEN_W, y + 1, rgb(10, 12, 30));
    }
}

/* ---------------- Dekoratif çerçeve ---------------- */

static void draw_outer_frame(void)
{
    int m = OUTER_MARGIN;

    /* Dış ince çizgi */
    video_draw_box(m,     m,     SCREEN_W - m,     SCREEN_H - m,     C_RULE);
    /* İç ince çizgi */
    video_draw_box(m + 3, m + 3, SCREEN_W - m - 3, SCREEN_H - m - 3, rgb(24, 26, 54));

    /* Köşe süsleri */
    int cs = 22;    /* köşe uzunluğu */
    int ct = 2;     /* köşe kalınlığı */

    /* Sol üst */
    video_fill_box(m + 3, m + 3, m + 3 + cs, m + 3 + ct, C_RULE_HI);
    video_fill_box(m + 3, m + 3, m + 3 + ct, m + 3 + cs, C_RULE_HI);

    /* Sağ üst */
    video_fill_box(SCREEN_W - m - 3 - cs, m + 3, SCREEN_W - m - 3, m + 3 + ct, C_RULE_HI);
    video_fill_box(SCREEN_W - m - 3 - ct, m + 3, SCREEN_W - m - 3, m + 3 + cs, C_RULE_HI);

    /* Sol alt */
    video_fill_box(m + 3, SCREEN_H - m - 3 - ct, m + 3 + cs, SCREEN_H - m - 3, C_RULE_HI);
    video_fill_box(m + 3, SCREEN_H - m - 3 - cs, m + 3 + ct, SCREEN_H - m - 3, C_RULE_HI);

    /* Sağ alt */
    video_fill_box(SCREEN_W - m - 3 - cs, SCREEN_H - m - 3 - ct, SCREEN_W - m - 3, SCREEN_H - m - 3, C_RULE_HI);
    video_fill_box(SCREEN_W - m - 3 - ct, SCREEN_H - m - 3 - cs, SCREEN_W - m - 3, SCREEN_H - m - 3, C_RULE_HI);
}

/* ---------------- Panel (bevel) çizimi ---------------- */

static void draw_panel(int x, int y, int w, int h, color_t fill)
{
    /* Gölge */
    video_fill_box(x + 4, y + 4, x + w + 4, y + h + 4, C_PANEL_SHADOW);
    /* Dış koyu kenar */
    video_fill_box(x - 2, y - 2, x + w + 2, y + h + 2, C_PANEL_DARK);
    /* Açık bevel */
    video_fill_box(x - 1, y - 1, x + w + 1, y + h + 1, C_PANEL_LIGHT);
    /* Dolgu */
    video_fill_box(x, y, x + w, y + h, fill);
}

/* Panel başlık şeridi */
static void draw_panel_header(int x, int y, int w, int h, const char *label)
{
    /* Arka plan */
    video_fill_box(x, y, x + w, y + h, C_BAR_BG);
    /* Üst aksan */
    video_fill_box(x, y, x + w, y + 2, C_BAR_EDGE);
    /* Alt ince çizgi */
    video_fill_box(x, y + h - 1, x + w, y + h, C_BAR_EDGE);

    /* Sol ve sağ köşe elmasları */
    video_fill_box(x + 4, y + h / 2 - 1, x + 8,  y + h / 2 + 2, C_BAR_EDGE);
    video_fill_box(x + w - 8, y + h / 2 - 1, x + w - 4, y + h / 2 + 2, C_BAR_EDGE);

    /* Metin */
    video_draw_debug_text(x + 14, y + (h - 8) / 2, C_TITLE_GLOW, "%s", label);
}

/* ---------------- Header ---------------- */

static void draw_header(void)
{
    int y = HEADER_Y;
    int h = HEADER_H;

    /* Header arka planı — ince bir panel */
    video_fill_box(OUTER_MARGIN + 6, y, SCREEN_W - OUTER_MARGIN - 6, y + h, rgb(10, 10, 26));
    video_fill_box(OUTER_MARGIN + 6, y,          SCREEN_W - OUTER_MARGIN - 6, y + 2, C_RULE_HI);
    video_fill_box(OUTER_MARGIN + 6, y + h - 2,  SCREEN_W - OUTER_MARGIN - 6, y + h, C_RULE);

    /* Ana başlık */
    text_center_shadow(y + 12, C_TITLE, C_TITLE_SHD,
                       "N E S   E M U L A T O R");

    /* Başlık altındaki süs çizgisi */
    int dw = 300;
    int dx0 = (SCREEN_W - dw) / 2;
    int dx1 = dx0 + dw;
    int dy = y + 34;
    video_fill_box(dx0, dy, dx1, dy + 1, C_RULE);
    /* Ortada elmas */
    video_fill_box((SCREEN_W / 2) - 3, dy - 2, (SCREEN_W / 2) + 3, dy + 3, C_TITLE);

    /* Alt yazı */
    text_center(y + 42, C_SUBTITLE, "SELECT A GAME TO BEGIN");

    /* Sağ üst köşede sürüm etiketi */
    video_draw_debug_text(SCREEN_W - 90, y + 8, C_DIM, "v0.2");

    /* Sol üst köşede durum ışığı */
    int lc = pulse(30);
    color_t lamp = rgb(60 + lc * 3, 200 + lc, 120);
    video_fill_box(OUTER_MARGIN + 16, y + 10, OUTER_MARGIN + 22, y + 16, lamp);
    video_fill_box(OUTER_MARGIN + 17, y + 11, OUTER_MARGIN + 21, y + 15, rgb(255, 255, 255));
}

/* ---------------- ROM tarama ---------------- */

static void scan_romfs(void)
{
    DIR *dir = opendir("rom://");
    struct dirent *entry;
    g_rom_count = 0;

    while (dir && (entry = readdir(dir)) != NULL && g_rom_count < MAX_ROMS) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".nes") == 0) {
            strncpy(g_rom_names[g_rom_count], entry->d_name, ROM_NAME_LEN - 1);
            g_rom_names[g_rom_count][ROM_NAME_LEN - 1] = '\0';
            g_rom_count++;
        }
    }
    if (dir) closedir(dir);
}

/* ---------------- Kaydırma ---------------- */

static void update_scroll(int selected)
{
    if (g_rom_count <= 0) { g_scroll_offset = 0; return; }

    if (selected < g_scroll_offset) {
        g_scroll_offset = selected;
    } else if (selected >= g_scroll_offset + VISIBLE_ITEMS) {
        g_scroll_offset = selected - VISIBLE_ITEMS + 1;
    }

    int max_scroll = g_rom_count - VISIBLE_ITEMS;
    if (max_scroll < 0)                max_scroll = 0;
    if (g_scroll_offset > max_scroll)  g_scroll_offset = max_scroll;
    if (g_scroll_offset < 0)           g_scroll_offset = 0;
}

/* ---------------- Liste paneli ---------------- */

static void draw_selection_row(int x, int y, int w, int h)
{
    /* Gradient dolgu — 6 yatay şerit */
    const int strips = 6;
    int sw = w / strips;

    for (int i = 0; i < strips; i++) {
        int t = i * 255 / (strips - 1);
        int r = (20 * (255 - t) + 60 * t) / 255;
        int g = (45 * (255 - t) + 110 * t) / 255;
        int b = (120 * (255 - t) + 200 * t) / 255;
        int x0 = x + i * sw;
        int x1 = (i == strips - 1) ? (x + w) : (x + (i + 1) * sw);
        video_fill_box(x0, y, x1, y + h, rgb(r, g, b));
    }

    /* Sol altın aksan çubuğu */
    int glow = pulse(20);
    color_t acc = rgb(255, 180 + glow, 60 + glow);
    video_fill_box(x, y, x + 4, y + h, acc);

    /* Üst ve alt parlak çizgi */
    video_fill_box(x, y, x + w, y + 1, rgb(180, 220, 255));
    video_fill_box(x, y + h - 1, x + w, y + h, rgb(80, 130, 220));
}

static void draw_list_panel(int selected)
{
    int x = LIST_X;
    int y = BODY_Y;
    int w = LIST_W;
    int h = BODY_H;

    draw_panel(x, y, w, h, C_PANEL_FILL);
    draw_panel_header(x, y, w, LIST_HEADER_H, "GAME LIBRARY");

    int content_y = y + LIST_HEADER_H + 4;
    int content_h = h - LIST_HEADER_H - 8;

    if (g_rom_count == 0) {
        text_center(content_y + content_h / 2 - 4, rgb(255, 130, 130),
                    "NO ROMS FOUND IN rom://");
        return;
    }

    /* Görünür ROM'ları çiz */
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = g_scroll_offset + i;
        if (idx >= g_rom_count) break;

        int iy = content_y + i * ITEM_H;
        int ih = ITEM_H - 2;
        int is_sel = (idx == selected);

        /* Alternatif satır arkaplanı */
        if (!is_sel && (idx & 1)) {
            video_fill_box(x + 4, iy, x + w - 4, iy + ih, C_ITEM_ALT);
        }

        /* Seçili satır */
        if (is_sel) {
            draw_selection_row(x + 4, iy, w - 8, ih);
        }

        /* Ayırıcı çizgi */
        if (idx != g_rom_count - 1) {
            video_fill_box(x + 8, iy + ih, x + w - 8, iy + ih + 1, C_ITEM_SEP);
        }

        /* İçerik */
        int tx = x + 20;
        int ty = iy + (ih - 8) / 2;

        /* Sıra numarası */
        char idxbuf[8];
        snprintf(idxbuf, sizeof(idxbuf), "%02d", idx + 1);
        video_draw_debug_text(tx, ty,
                              is_sel ? rgb(255, 220, 130) : C_ITEM_NUM,
                              "%s", idxbuf);

        /* Seçim oku */
        if (is_sel) {
            video_draw_debug_text(tx + 26, ty, rgb(255, 255, 180), ">");
        }

        /* ROM adı (kırp) */
        char namebuf[ROM_NAME_LEN];
        strncpy(namebuf, g_rom_names[idx], ROM_NAME_LEN - 1);
        namebuf[ROM_NAME_LEN - 1] = '\0';

        int max_chars = (w - 90) / 8;
        if (max_chars < 4) max_chars = 4;
        if ((int)strlen(namebuf) > max_chars) {
            namebuf[max_chars - 3] = '.';
            namebuf[max_chars - 2] = '.';
            namebuf[max_chars - 1] = '.';
            namebuf[max_chars]     = '\0';
        }

        video_draw_debug_text(tx + 44, ty,
                              is_sel ? C_ITEM_SEL_FG : C_ITEM,
                              "%s", namebuf);
    }

    /* Kaydırma okları */
    if (g_scroll_offset > 0) {
        int a = pulse(15);
        video_draw_debug_text(x + w - 30, y + LIST_HEADER_H - 14,
                              rgb(160 + a, 160 + a, 220),
                              "^");
    }
    if (g_scroll_offset + VISIBLE_ITEMS < g_rom_count) {
        int a = pulse(15);
        video_draw_debug_text(x + w - 30, y + h - 12,
                              rgb(160 + a, 160 + a, 220),
                              "v");
    }
}

/* ---------------- Kartuş ikonu (sağ panel) ---------------- */

static void draw_cartridge_icon(int cx, int cy)
{
    int w = 96;
    int h = 64;
    int x = cx - w / 2;
    int y = cy - h / 2;

    /* Gölge */
    video_fill_box(x + 3, y + 3, x + w + 3, y + h + 3, rgb(0, 0, 0));

    /* Gövde */
    video_fill_box(x, y, x + w, y + h, C_CART_BODY);
    /* Kenar */
    video_draw_box(x, y, x + w, y + h, C_CART_EDGE);

    /* Üst kabartma */
    video_fill_box(x + 2, y + 2, x + w - 2, y + 4, C_CART_LABEL);

    /* Etiket alanı */
    video_fill_box(x + 8, y + 12, x + w - 8, y + h - 14, C_CART_STRIPE);
    video_draw_box(x + 8, y + 12, x + w - 8, y + h - 14, C_CART_EDGE);

    /* Etiket üzerinde dekoratif çizgiler */
    for (int i = 0; i < 3; i++) {
        int ly = y + 18 + i * 8;
        video_fill_box(x + 14, ly, x + w - 14, ly + 1, C_CART_LABEL);
    }

    /* Altın kontaklar (alt kenar) */
    int stripe_y = y + h - 10;
    for (int i = 0; i < 8; i++) {
        int sx = x + 8 + i * 10;
        video_fill_box(sx, stripe_y, sx + 5, y + h - 3, rgb(220, 200, 120));
    }
}

/* ---------------- Sağ bilgi paneli ---------------- */

static void draw_info_panel(int selected)
{
    int x = INFO_X;
    int y = BODY_Y;
    int w = INFO_W;
    int h = BODY_H;

    draw_panel(x, y, w, h, C_PANEL_FILL2);
    draw_panel_header(x, y, w, LIST_HEADER_H, "CARTRIDGE");

    if (g_rom_count == 0 || selected < 0 || selected >= g_rom_count) {
        text_center(y + h / 2, C_DIM, "---");
        return;
    }

    int cy = y + LIST_HEADER_H + 50;
    draw_cartridge_icon(x + w / 2, cy);

    int ky = cy + 48;
    int lx = x + 12;

    /* FILE */
    video_draw_debug_text(lx, ky, C_INFO_KEY, "FILE");
    video_fill_box(lx, ky + 10, x + w - 12, ky + 11, C_RULE);
    ky += 18;

    /* Dosya adı — 2 satıra kadar sar */
    {
        const char *name = g_rom_names[selected];
        int maxc = (w - 24) / 8;
        if (maxc < 4) maxc = 4;

        char line1[ROM_NAME_LEN];
        char line2[ROM_NAME_LEN];
        strncpy(line1, name, sizeof(line1) - 1);
        line1[sizeof(line1) - 1] = '\0';
        line2[0] = '\0';

        if ((int)strlen(line1) > maxc) {
            int cut = maxc;
            /* Kelime sınırı bul */
            for (int i = cut; i > 0; i--) {
                if (line1[i] == ' ' || line1[i] == '_' || line1[i] == '-') {
                    cut = i;
                    break;
                }
            }
            strncpy(line2, line1 + cut, sizeof(line2) - 1);
            line2[sizeof(line2) - 1] = '\0';
            line1[cut] = '\0';
        }

        video_draw_debug_text(lx, ky, C_INFO_VAL, "%s", line1);
        ky += 12;
        if (line2[0]) {
            video_draw_debug_text(lx, ky, C_INFO_VAL, "%s", line2);
            ky += 12;
        }
    }

    ky += 8;
    video_draw_debug_text(lx, ky, C_INFO_KEY, "INDEX");
    video_fill_box(lx, ky + 10, x + w - 12, ky + 11, C_RULE);
    ky += 18;

    char idxbuf[32];
    snprintf(idxbuf, sizeof(idxbuf), "%d / %d", selected + 1, g_rom_count);
    video_draw_debug_text(lx, ky, C_INFO_VAL, "%s", idxbuf);
    ky += 22;

    video_draw_debug_text(lx, ky, C_INFO_KEY, "STATUS");
    video_fill_box(lx, ky + 10, x + w - 12, ky + 11, C_RULE);
    ky += 18;

    /* Yanıp sönen READY etiketi */
    int p = pulse(20);
    color_t ready = rgb(60 + p * 2, 200 + p / 2, 100 + p);
    video_fill_box(lx, ky + 1, lx + 6, ky + 7, ready);
    video_draw_debug_text(lx + 12, ky, C_INFO_VAL, "READY");
}

/* ---------------- Sayaç / progress bar ---------------- */

static void draw_counter(int selected)
{
    if (g_rom_count <= 1) return;

    int y = COUNTER_Y;
    int h = COUNTER_H;
    int x0 = SCREEN_W / 2 - 180;
    int x1 = SCREEN_W / 2 + 180;

    /* Arka plan */
    video_fill_box(x0 - 2, y - 2, x1 + 2, y + h + 2, C_PANEL_DARK);
    video_fill_box(x0, y, x1, y + h, C_PROG_BG);

    /* Dolgu */
    int filled = (x1 - x0) * (selected + 1) / g_rom_count;
    if (filled > 0) {
        /* Gradient dolgu — 3 şerit */
        for (int i = 0; i < 3; i++) {
            int t = i * 255 / 2;
            int r = (40 * (255 - t) + 80 * t) / 255;
            int g = (120 * (255 - t) + 200 * t) / 255;
            int b = (220 * (255 - t) + 255 * t) / 255;
            int sx0 = x0 + filled * i / 3;
            int sx1 = x0 + filled * (i + 1) / 3;
            video_fill_box(sx0, y + 1, sx1, y + h - 1, rgb(r, g, b));
        }
    }

    /* Kenar */
    video_draw_box(x0, y, x1, y + h, C_PROG_EDGE);

    /* Ortada metin */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", selected + 1, g_rom_count);
    int tw = text_w(buf);
    int tx = (SCREEN_W - tw) / 2;
    int ty = y + (h - 8) / 2;
    /* Hafif kontur için siyah arka */
    video_fill_box(tx - 4, ty - 2, tx + tw + 4, ty + 10, rgb(0, 0, 0));
    video_draw_debug_text(tx, ty, rgb(220, 240, 255), "%s", buf);
}

/* ---------------- Footer ---------------- */

static void draw_footer(void)
{
    int y = FOOTER_Y;
    int h = FOOTER_H;
    int m = OUTER_MARGIN + 6;

    video_fill_box(m, y, SCREEN_W - m, y + h, C_FOOTER_BG);
    video_fill_box(m, y, SCREEN_W - m, y + 2, C_RULE_HI);
    video_fill_box(m, y + h - 1, SCREEN_W - m, y + h, C_RULE);

    /* Üç bölmeli ipucu */
    int t0 = SCREEN_W / 3;
    int t1 = (SCREEN_W * 2) / 3;

    /* Dikey ayırıcılar */
    video_fill_box(t0, y + 4, t0 + 1, y + h - 4, C_RULE);
    video_fill_box(t1, y + 4, t1 + 1, y + h - 4, C_RULE);

    /* Bölüm 1: Navigate */
    int ytext = y + (h - 8) / 2;
    const char *s1a = "UP/DOWN";
    const char *s1b = "Navigate";
    int w1 = text_w(s1a) + 12 + text_w(s1b);
    int x1 = (t0 - w1) / 2;
    video_draw_debug_text(x1, ytext, C_HINT, "%s", s1a);
    video_draw_debug_text(x1 + text_w(s1a) + 12, ytext, C_FOOTER_TXT, "%s", s1b);

    /* Bölüm 2: Load */
    const char *s2a = "START";
    const char *s2b = "Load ROM";
    int w2 = text_w(s2a) + 12 + text_w(s2b);
    int x2 = t0 + (t1 - t0 - w2) / 2;
    video_draw_debug_text(x2, ytext, C_HINT, "%s", s2a);
    video_draw_debug_text(x2 + text_w(s2a) + 12, ytext, C_FOOTER_TXT, "%s", s2b);

    /* Bölüm 3: Reset */
    const char *s3a = "B5";
    const char *s3b = "Reset";
    int w3 = text_w(s3a) + 12 + text_w(s3b);
    int x3 = t1 + (SCREEN_W - m - t1 - w3) / 2;
    video_draw_debug_text(x3, ytext, C_HINT, "%s", s3a);
    video_draw_debug_text(x3 + text_w(s3a) + 12, ytext, C_FOOTER_TXT, "%s", s3b);
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

void rom_browser_init(void)
{
    romfs_init_default();
    scan_romfs();
    g_scroll_offset = 0;
    g_frame         = 0;
}

int rom_browser_count(void)
{
    return g_rom_count;
}

const char *rom_browser_name(int index)
{
    if (index < 0 || index >= g_rom_count) return NULL;
    return g_rom_names[index];
}

void rom_browser_draw(int selected)
{
    g_frame++;

    if (g_rom_count > 0) {
        if (selected < 0)            selected = 0;
        if (selected >= g_rom_count) selected = g_rom_count - 1;
    } else {
        selected = 0;
    }

    update_scroll(selected);

    draw_background();
    draw_starfield();
    draw_scan_hints();
    draw_outer_frame();
    draw_header();

    draw_list_panel(selected);
    draw_info_panel(selected);

    draw_counter(selected);
    draw_footer();
}