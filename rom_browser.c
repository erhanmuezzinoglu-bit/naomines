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
static font_t *font_14pt = NULL;

static texture_description_t *frame_blue_tex = NULL;
static texture_description_t *frame_side_tex = NULL;

/* Footer Buton Dokuları */
static texture_description_t *btn_b_tex = NULL;
static texture_description_t *btn_dpad_tex = NULL;
static texture_description_t *btn_a_tex = NULL;
static texture_description_t *btn_select_tex = NULL;
static texture_description_t *btn_start_tex = NULL;

/* Eski Kapaklar */
static texture_description_t *cover_mario_tex = NULL;
static texture_description_t *cover_zelda_tex = NULL;
static texture_description_t *cover_castlevania_tex = NULL;
static texture_description_t *cover_punchout_tex = NULL;
static texture_description_t *cover_smb3_tex = NULL;

/* Yeni Kapaklar */
static texture_description_t *cover_streemerz_tex = NULL;
static texture_description_t *cover_castlevania3_tex = NULL;
static texture_description_t *cover_crazyclimber_tex = NULL;
static texture_description_t *cover_crystalmines_tex = NULL;
static texture_description_t *cover_deadlytowers_tex = NULL;
static texture_description_t *cover_duckhunt_tex = NULL;
static texture_description_t *cover_excitebike_tex = NULL;
static texture_description_t *cover_gargoyle2_tex = NULL;
static texture_description_t *cover_holydiver_tex = NULL;
static texture_description_t *cover_kirby_tex = NULL;
static texture_description_t *cover_sisyphus_tex = NULL;
static texture_description_t *cover_marble_tex = NULL;
static texture_description_t *cover_megaman2_tex = NULL;
static texture_description_t *cover_metroid_tex = NULL;
static texture_description_t *cover_prince_tex = NULL;
static texture_description_t *cover_gradius_tex = NULL;

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

/* Footer Buton Sembolleri */
extern uint8_t *btn_b_png_data;
extern unsigned int btn_b_png_width;
extern unsigned int btn_b_png_height;

extern uint8_t *btn_dpad_png_data;
extern unsigned int btn_dpad_png_width;
extern unsigned int btn_dpad_png_height;

extern uint8_t *btn_a_png_data;
extern unsigned int btn_a_png_width;
extern unsigned int btn_a_png_height;

extern uint8_t *btn_select_png_data;
extern unsigned int btn_select_png_width;
extern unsigned int btn_select_png_height;

extern uint8_t *btn_start_png_data;
extern unsigned int btn_start_png_width;
extern unsigned int btn_start_png_height;

/* Eski Kapak Sembolleri */
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

/* Yeni Kapak Sembolleri */
extern uint8_t *cover_streemerz_png_data;
extern unsigned int cover_streemerz_png_width;
extern unsigned int cover_streemerz_png_height;

extern uint8_t *cover_castlevania3_png_data;
extern unsigned int cover_castlevania3_png_width;
extern unsigned int cover_castlevania3_png_height;

extern uint8_t *cover_crazyclimber_png_data;
extern unsigned int cover_crazyclimber_png_width;
extern unsigned int cover_crazyclimber_png_height;

extern uint8_t *cover_crystalmines_png_data;
extern unsigned int cover_crystalmines_png_width;
extern unsigned int cover_crystalmines_png_height;

extern uint8_t *cover_deadlytowers_png_data;
extern unsigned int cover_deadlytowers_png_width;
extern unsigned int cover_deadlytowers_png_height;

extern uint8_t *cover_duckhunt_png_data;
extern unsigned int cover_duckhunt_png_width;
extern unsigned int cover_duckhunt_png_height;

extern uint8_t *cover_excitebike_png_data;
extern unsigned int cover_excitebike_png_width;
extern unsigned int cover_excitebike_png_height;

extern uint8_t *cover_gargoyle2_png_data;
extern unsigned int cover_gargoyle2_png_width;
extern unsigned int cover_gargoyle2_png_height;

extern uint8_t *cover_holydiver_png_data;
extern unsigned int cover_holydiver_png_width;
extern unsigned int cover_holydiver_png_height;

extern uint8_t *cover_kirby_png_data;
extern unsigned int cover_kirby_png_width;
extern unsigned int cover_kirby_png_height;

extern uint8_t *cover_sisyphus_png_data;
extern unsigned int cover_sisyphus_png_width;
extern unsigned int cover_sisyphus_png_height;

extern uint8_t *cover_marble_png_data;
extern unsigned int cover_marble_png_width;
extern unsigned int cover_marble_png_height;

extern uint8_t *cover_megaman2_png_data;
extern unsigned int cover_megaman2_png_width;
extern unsigned int cover_megaman2_png_height;

extern uint8_t *cover_metroid_png_data;
extern unsigned int cover_metroid_png_width;
extern unsigned int cover_metroid_png_height;

extern uint8_t *cover_prince_png_data;
extern unsigned int cover_prince_png_width;
extern unsigned int cover_prince_png_height;

extern uint8_t *cover_gradius_png_data;
extern unsigned int cover_gradius_png_width;
extern unsigned int cover_gradius_png_height;


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
    
    if (btn_b_png_data) btn_b_tex = ta_texture_desc_malloc_direct(btn_b_png_width, btn_b_png_data, TA_TEXTUREMODE_ARGB4444);
    if (btn_dpad_png_data) btn_dpad_tex = ta_texture_desc_malloc_direct(btn_dpad_png_width, btn_dpad_png_data, TA_TEXTUREMODE_ARGB4444);
    if (btn_a_png_data) btn_a_tex = ta_texture_desc_malloc_direct(btn_a_png_width, btn_a_png_data, TA_TEXTUREMODE_ARGB4444);
    if (btn_select_png_data) btn_select_tex = ta_texture_desc_malloc_direct(btn_select_png_width, btn_select_png_data, TA_TEXTUREMODE_ARGB4444);
    if (btn_start_png_data) btn_start_tex = ta_texture_desc_malloc_direct(btn_start_png_width, btn_start_png_data, TA_TEXTUREMODE_ARGB4444);

    if (cover_mario_png_data) cover_mario_tex = ta_texture_desc_malloc_direct(cover_mario_png_width, cover_mario_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_zelda_png_data) cover_zelda_tex = ta_texture_desc_malloc_direct(cover_zelda_png_width, cover_zelda_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_castlevania_png_data) cover_castlevania_tex = ta_texture_desc_malloc_direct(cover_castlevania_png_width, cover_castlevania_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_punchout_png_data) cover_punchout_tex = ta_texture_desc_malloc_direct(cover_punchout_png_width, cover_punchout_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_smb3_png_data) cover_smb3_tex = ta_texture_desc_malloc_direct(cover_smb3_png_width, cover_smb3_png_data, TA_TEXTUREMODE_ARGB4444);

    if (cover_streemerz_png_data) cover_streemerz_tex = ta_texture_desc_malloc_direct(cover_streemerz_png_width, cover_streemerz_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_castlevania3_png_data) cover_castlevania3_tex = ta_texture_desc_malloc_direct(cover_castlevania3_png_width, cover_castlevania3_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_crazyclimber_png_data) cover_crazyclimber_tex = ta_texture_desc_malloc_direct(cover_crazyclimber_png_width, cover_crazyclimber_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_crystalmines_png_data) cover_crystalmines_tex = ta_texture_desc_malloc_direct(cover_crystalmines_png_width, cover_crystalmines_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_deadlytowers_png_data) cover_deadlytowers_tex = ta_texture_desc_malloc_direct(cover_deadlytowers_png_width, cover_deadlytowers_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_duckhunt_png_data) cover_duckhunt_tex = ta_texture_desc_malloc_direct(cover_duckhunt_png_width, cover_duckhunt_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_excitebike_png_data) cover_excitebike_tex = ta_texture_desc_malloc_direct(cover_excitebike_png_width, cover_excitebike_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_gargoyle2_png_data) cover_gargoyle2_tex = ta_texture_desc_malloc_direct(cover_gargoyle2_png_width, cover_gargoyle2_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_holydiver_png_data) cover_holydiver_tex = ta_texture_desc_malloc_direct(cover_holydiver_png_width, cover_holydiver_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_kirby_png_data) cover_kirby_tex = ta_texture_desc_malloc_direct(cover_kirby_png_width, cover_kirby_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_sisyphus_png_data) cover_sisyphus_tex = ta_texture_desc_malloc_direct(cover_sisyphus_png_width, cover_sisyphus_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_marble_png_data) cover_marble_tex = ta_texture_desc_malloc_direct(cover_marble_png_width, cover_marble_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_megaman2_png_data) cover_megaman2_tex = ta_texture_desc_malloc_direct(cover_megaman2_png_width, cover_megaman2_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_metroid_png_data) cover_metroid_tex = ta_texture_desc_malloc_direct(cover_metroid_png_width, cover_metroid_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_prince_png_data) cover_prince_tex = ta_texture_desc_malloc_direct(cover_prince_png_width, cover_prince_png_data, TA_TEXTUREMODE_ARGB4444);
    if (cover_gradius_png_data) cover_gradius_tex = ta_texture_desc_malloc_direct(cover_gradius_png_width, cover_gradius_png_data, TA_TEXTUREMODE_ARGB4444);

#ifdef FEATURE_FREETYPE
    extern uint8_t *dejavusans_ttf_data;
    extern unsigned int dejavusans_ttf_len;
    
    font_18pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
    if (font_18pt) font_set_size(font_18pt, 18);
    
    font_14pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
    if (font_14pt) font_set_size(font_14pt, 14);
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
    if (strstr(name, "Punch") != NULL) return cover_punchout_tex;
    
    if (strstr(name, "Streemerz") != NULL) return cover_streemerz_tex;
    if (strstr(name, "Castlevania III") != NULL || strstr(name, "Dracula") != NULL) return cover_castlevania3_tex;
    if (strstr(name, "Castlevania") != NULL) return cover_castlevania_tex;
    if (strstr(name, "Crazy") != NULL) return cover_crazyclimber_tex;
    if (strstr(name, "Crystal") != NULL) return cover_crystalmines_tex;
    if (strstr(name, "Deadly") != NULL) return cover_deadlytowers_tex;
    if (strstr(name, "Duck") != NULL) return cover_duckhunt_tex;
    if (strstr(name, "Excitebike") != NULL) return cover_excitebike_tex;
    if (strstr(name, "Gargoyle") != NULL) return cover_gargoyle2_tex;
    if (strstr(name, "Holy Diver") != NULL) return cover_holydiver_tex;
    if (strstr(name, "Kirby") != NULL) return cover_kirby_tex;
    if (strstr(name, "Sisyphus") != NULL) return cover_sisyphus_tex;
    if (strstr(name, "Marble") != NULL) return cover_marble_tex;
    if (strstr(name, "Mega Man 2") != NULL) return cover_megaman2_tex;
    if (strstr(name, "Metroid") != NULL) return cover_metroid_tex;
    if (strstr(name, "Prince") != NULL) return cover_prince_tex;
    if (strstr(name, "Gradius") != NULL) return cover_gradius_tex;

    return cover_mario_tex;
}

/* Footer Butonlarını Çizen Fonksiyon */
static void draw_footer(void)
{
    int y_center = 468; 
    
    float h_dpad_a_b = 18.0f;
    float h_select_start = 42.0f;

    /* Sadece yazı rengini kırmızıya (rgb(255, 0, 0)) çevirdik. */

    // 1. D-Pad (Navigate)
    if (btn_dpad_tex) {
        float s = h_dpad_a_b / (float)btn_dpad_png_height;
        int w = (int)(btn_dpad_png_width * s);
        sprite_draw_scaled(20, y_center - (int)(h_dpad_a_b / 2), s, s, btn_dpad_tex);
        if (font_14pt) ta_draw_text(20 + w + 8, y_center - 7, font_14pt, rgb(255, 0, 0), "Navigate");
    }

    // 2. A Button (Select)
    if (btn_a_tex) {
        float s = h_dpad_a_b / (float)btn_a_png_height;
        int w = (int)(btn_a_png_width * s);
        sprite_draw_scaled(120, y_center - (int)(h_dpad_a_b / 2), s, s, btn_a_tex);
        if (font_14pt) ta_draw_text(120 + w + 8, y_center - 7, font_14pt, rgb(255, 0, 0), "Select");
    }

    // 3. Select Button (Menu)
    if (btn_select_tex) {
        float s = h_select_start / (float)btn_select_png_height;
        s *= 1.15f; 
        int w = (int)(btn_select_png_width * s);
        int h = (int)(btn_select_png_height * s);
        sprite_draw_scaled(230, y_center - (h / 2), s, s, btn_select_tex);
        if (font_14pt) ta_draw_text(230 + w + 8, y_center - 7, font_14pt, rgb(255, 0, 0), "Menu");
    }

    // 4. Start Button (Start)
    if (btn_start_tex) {
        float s = h_select_start / (float)btn_start_png_height;
        int w = (int)(btn_start_png_width * s);
        int h = (int)(btn_start_png_height * s);
        sprite_draw_scaled(370, y_center - (h / 2), s, s, btn_start_tex);
        if (font_14pt) ta_draw_text(370 + w + 8, y_center - 7, font_14pt, rgb(255, 0, 0), "Start");
    }

    // 5. B Button (Back)
    if (btn_b_tex) {
        float s = h_dpad_a_b / (float)btn_b_png_height;
        int w = (int)(btn_b_png_width * s);
        sprite_draw_scaled(540, y_center - (int)(h_dpad_a_b / 2), s, s, btn_b_tex);
        if (font_14pt) ta_draw_text(540 + w + 8, y_center - 7, font_14pt, rgb(255, 0, 0), "Back");
    }
}

void rom_browser_draw(int selected)
{
    if (g_rom_count > 0) {
        if (selected < 0) selected = 0;
        if (selected >= g_rom_count) selected = g_rom_count - 1;
    } else {
        selected = 0;
    }

    if (target_selected == -1) {
        target_selected = selected;
        old_selected = selected;
    }

    if (selected != target_selected) {
        target_selected = selected;
        anim_progress = 0.0f;
    }

    if (anim_progress < 1.0f) {
        anim_progress += 0.15f; 
        if (anim_progress >= 1.0f) {
            anim_progress = 1.0f;
            old_selected = target_selected; 
        }
    }

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

        for (int i = -3; i <= 3; i++) {
            int idx = (old_selected + i + g_rom_count) % g_rom_count;
            texture_description_t *tex = get_cover_for_index(idx);
            if (!tex) continue;

            float start_x = center_x + i * side_offset;
            float end_x = center_x + (i - direction) * side_offset;
            float current_x = start_x + (end_x - start_x) * anim_progress;

            float start_scale = 0.75f - 0.25f * fabsf((float)i);
            float end_scale = 0.75f - 0.25f * fabsf((float)(i - direction));
            float current_scale = start_scale + (end_scale - start_scale) * anim_progress;

            if (fabsf(current_x - center_x) > side_offset * 2.5f) continue;

            int dw = (int)(cover_mario_png_width * current_scale);
            int dh = (int)(cover_mario_png_height * current_scale);

            sprite_draw_scaled(current_x - (dw / 2), center_y - (dh / 2), current_scale, current_scale, tex);

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

    draw_footer();

    ta_commit_end();
    ta_render();
}