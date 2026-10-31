#include "rom_browser.h"
#include <naomi/video.h>
#include <naomi/romfs.h>
#include <naomi/ta.h>
#include <naomi/sprite/sprite.h>
#include <naomi/ta-freetype.h>
#include <naomi/font.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define MAX_ROMS        128
#define ROM_NAME_LEN    64
#define SCREEN_W        640
#define SCREEN_H        480

static char g_rom_names[MAX_ROMS][ROM_NAME_LEN];
static int  g_rom_count = 0;

static texture_description_t *bg_tex = NULL;
static font_t *font_18pt = NULL;

static texture_description_t *frame_blue_tex = NULL;
static texture_description_t *frame_side_tex = NULL;
static texture_description_t *cover_mario_tex = NULL;
static texture_description_t *cover_zelda_tex = NULL;
static texture_description_t *cover_castlevania_tex = NULL;
static texture_description_t *cover_punchout_tex = NULL;
static texture_description_t *cover_smb3_tex = NULL;

/* Animasyon Değişkenleri */
static int target_selected = -1;
static int old_selected = 0;
static float anim_progress = 1.0f;

/* Semboller */
extern uint8_t *background_png_data;
extern unsigned int background_png_width;
extern unsigned int background_png_height;

extern uint8_t *frame_blue_png_data;
extern unsigned int frame_blue_png_width;
extern unsigned int frame_blue_png_height;

extern uint8_t *frame_side_png_data;
extern unsigned int frame_side_png_width;
extern unsigned int frame_side_png_height;

extern uint8_t *cover_mario_png_data;
extern unsigned int cover_mario_png_width;
extern unsigned int cover_mario_png_height;

extern uint8_t *cover_zelda_png_data;
extern unsigned int cover_zelda_png_width;
extern unsigned int cover_zelda_png_height;

extern uint8_t *cover_castlevania_png_data;
extern unsigned int cover_castlevania_png_width;
extern unsigned int cover_castlevania_png_height;

extern uint8_t *cover_punchout_png_data;
extern unsigned int cover_punchout_png_width;
extern unsigned int cover_punchout_png_height;

extern uint8_t *cover_smb3_png_data;
extern unsigned int cover_smb3_png_width;
extern unsigned int cover_smb3_png_height;

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

void rom_browser_init(void)
{
    romfs_init_default();
    scan_romfs();

    if (background_png_data) bg_tex = ta_texture_desc_malloc_direct(background_png_width, background_png_data, TA_TEXTUREMODE_ARGB4444);
    if (frame_blue_png_data) frame_blue_tex = ta_texture_desc_malloc_direct(frame_blue_png_width, frame_blue_png_data, TA_TEXTUREMODE_ARGB4444);
    if (frame_side_png_data) frame_side_tex = ta_texture_desc_malloc_direct(frame_side_png_width, frame_side_png_data, TA_TEXTUREMODE_ARGB4444);
    
    if (cover_mario_png_data) cover_mario_tex = ta_texture_desc_malloc_direct(cover_mario_png_width, cover_mario_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_zelda_png_data) cover_zelda_tex = ta_texture_desc_malloc_direct(cover_zelda_png_width, cover_zelda_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_castlevania_png_data) cover_castlevania_tex = ta_texture_desc_malloc_direct(cover_castlevania_png_width, cover_castlevania_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_punchout_png_data) cover_punchout_tex = ta_texture_desc_malloc_direct(cover_punchout_png_width, cover_punchout_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_smb3_png_data) cover_smb3_tex = ta_texture_desc_malloc_direct(cover_smb3_png_width, cover_smb3_png_data, TA_TEXTUREMODE_ARGB4444);

#ifdef FEATURE_FREETYPE
    extern uint8_t *dejavusans_ttf_data;
    extern unsigned int dejavusans_ttf_len;
    font_18pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
    if (font_18pt) font_set_size(font_18pt, 18);
#endif
}

int rom_browser_count(void) { return g_rom_count; }

const char *rom_browser_name(int index)
{
    if (index < 0 || index >= g_rom_count) return NULL;
    return g_rom_names[index];
}

static texture_description_t *get_cover_for_index(int index)
{
    if (index < 0 || index >= g_rom_count) return NULL;
    const char *name = g_rom_names[index];

    if (strstr(name, "Bros. 3") != NULL || strstr(name, "Bros 3") != NULL) return cover_smb3_tex;
    if (strstr(name, "Mario") != NULL) return cover_mario_tex;
    if (strstr(name, "Zelda") != NULL) return cover_zelda_tex;
    if (strstr(name, "Castlevania") != NULL) return cover_castlevania_tex;
    if (strstr(name, "Punch") != NULL) return cover_punchout_tex;
    
    return cover_mario_tex;
}

void rom_browser_draw(int selected)
{
    if (g_rom_count > 0) {
        if (selected < 0) selected = 0;
        if (selected >= g_rom_count) selected = g_rom_count - 1;
    } else {
        selected = 0;
    }

    /* İlk çalışma kontrolü */
    if (target_selected == -1) {
        target_selected = selected;
        old_selected = selected;
    }

    /* Kullanıcı tuşa bastıysa yeni hedefi belirle ve animasyonu başlat */
    if (selected != target_selected) {
        target_selected = selected;
        anim_progress = 0.0f;
    }

    /* Animasyonu ilerlet */
    if (anim_progress < 1.0f) {
        anim_progress += 0.15f; /* Animasyon hızı (0.15 = 7 karede tamamlanır) */
        if (anim_progress >= 1.0f) {
            anim_progress = 1.0f;
            old_selected = target_selected; /* Animasyon bitti, merkez güncellendi */
        }
    }

    /* Yön Belirleme (Aşağı = 1 -> Sola, Yukarı = -1 -> Sağa) */
    int diff = target_selected - old_selected;
    if (diff > g_rom_count / 2) diff -= g_rom_count;
    if (diff < -g_rom_count / 2) diff += g_rom_count;
    int direction = (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);

    ta_commit_begin();

    if (bg_tex) {
        sprite_draw_simple(-192, -272, bg_tex);
    }

    if (g_rom_count > 0) {
        int center_x = 320;
        int center_y = 216;
        int side_offset = 220; 

        /* Şerit Mantığı: Eski merkezin etrafındaki kapakları çiz */
        for (int i = -3; i <= 3; i++) {
            int idx = (old_selected + i + g_rom_count) % g_rom_count;
            texture_description_t *tex = get_cover_for_index(idx);
            if (!tex) continue;

            /* Başlangıç ve Bitiş Pozisyonları */
            float start_x = center_x + i * side_offset;
            float end_x = center_x + (i - direction) * side_offset;
            float current_x = start_x + (end_x - start_x) * anim_progress;

            /* Başlangıç ve Bitiş Ölçekleri (Kenara gittikçe küçülür) */
            float start_scale = 0.75f - 0.25f * fabsf((float)i);
            float end_scale = 0.75f - 0.25f * fabsf((float)(i - direction));
            float current_scale = start_scale + (end_scale - start_scale) * anim_progress;

            /* Ekranın çok dışındaysa çizme (Performans için) */
            if (fabsf(current_x - center_x) > side_offset * 2.5f) continue;

            int dw = (int)(cover_mario_png_width * current_scale);
            int dh = (int)(cover_mario_png_height * current_scale);

            /* Kapağı Çiz */
            sprite_draw_scaled(current_x - (dw / 2), center_y - (dh / 2), current_scale, current_scale, tex);

            /* Çerçeve Çizimi: Merkeze en yakın olan Mavi, diğerleri Gri */
            if (fabsf(current_x - center_x) < side_offset * 0.5f) {
                if (frame_blue_tex) {
                    int fw = (int)(frame_blue_png_width * current_scale);
                    int fh = (int)(frame_blue_png_height * current_scale);
                    sprite_draw_scaled(current_x - (fw / 2), center_y - (fh / 2), current_scale, current_scale, frame_blue_tex);
                }
            } else {
                if (frame_side_tex) {
                    float s_scale = 1.0f; 
                    int fw = (int)(frame_side_png_width * s_scale);
                    int fh = (int)(frame_side_png_height * s_scale);
                    sprite_draw_scaled(current_x - (fw / 2), center_y - (fh / 2), s_scale, s_scale, frame_side_tex);
                }
            }
        }
    }

    if (g_rom_count == 0) {
#ifdef FEATURE_FREETYPE
        if (font_18pt) ta_draw_text(260, 75, font_18pt, rgb(255, 100, 100), "NO ROMS FOUND");
#endif
    } else {
        const char *name = g_rom_names[target_selected];
#ifdef FEATURE_FREETYPE
        if (font_18pt) {
            char display_name[ROM_NAME_LEN];
            strncpy(display_name, name, ROM_NAME_LEN - 1);
            display_name[ROM_NAME_LEN - 1] = '\0';
            char *dot = strrchr(display_name, '.');
            if (dot) *dot = '\0';

            int text_x = 320 - (strlen(display_name) * 4.5);
            ta_draw_text(text_x, 72, font_18pt, rgb(255, 255, 255), "%s", display_name);
        }
#endif
    }

    ta_commit_end();
    ta_render();
}