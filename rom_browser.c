#include "rom_browser.h"

#include <naomi/video.h>
#include <naomi/romfs.h>
#include <naomi/ta.h>
#include <naomi/sprite/sprite.h>
#include <naomi/ta-freetype.h>
#include <naomi/font.h>
#include <naomi/audio.h>

#include <xmp.h>

/*
 * vorbisfile.h normally defines four convenience callback tables as static
 * variables in every translation unit. This file uses ov_open()/ov_read(),
 * not those tables, so exclude them instead of weakening -Werror globally.
 */
#ifndef OV_EXCLUDE_STATIC_CALLBACKS
#define OV_EXCLUDE_STATIC_CALLBACKS
#endif
#include <vorbis/vorbisfile.h>

#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#define MAX_ROMS          128
#define ROM_NAME_LEN      122 /* 121 filename bytes + NUL; fits main.c fullpath[128]. */
#define COVER_CACHE_SIZE           7
#define COVER_TEXTURE_SIZE         512
#define COVER_TEXTURE_PIXELS       (COVER_TEXTURE_SIZE * COVER_TEXTURE_SIZE)
#define COVER_UPLOAD_ROWS_PER_FRAME 64

/*
 * The new background asset is a 512x512 texture containing a centered
 * 320x240 artwork area. Draw it at 2x so that the artwork still covers the
 * complete 640x480 screen. Compared with the previous 1024x1024 ARGB4444
 * texture this reduces background texture RAM usage from 2 MiB to 512 KiB.
 */
#define BACKGROUND_TEXTURE_SIZE    512
#define BACKGROUND_DRAW_SCALE      2.0f
#define BACKGROUND_DRAW_X          (-192)
#define BACKGROUND_DRAW_Y          (-272)

/* Background tracker music and menu selection sound are stored in ROMFS. */
#define MUSIC_FILE_PATH              "rom://theme.xm"
#define MENU_SFX_FILE_PATH           "rom://menuhighlight.ogg"

/* The tested menu music setting is 10% faster, with the same 44.1 kHz rate. */
#define MUSIC_SPEED_PERCENT          105u
#define MUSIC_DISPLAY_RATE            60u

/* OGG is decoded once at menu startup, then mixed into the tracker PCM. */
#define MENU_SFX_OUTPUT_RATE         44100u
#define MENU_SFX_MAX_SECONDS             3u
#define MENU_SFX_VOICE_COUNT              4u
#define MENU_SFX_GAIN_Q15             32768  /* 1.00; unity gain before mixing with music. */
#define MUSIC_MIX_BUFFER_BYTES        32768u

/*
 * Covers are kept at full 512x512 ARGB4444 resolution in a fixed VRAM pool.
 * Seven slots consume 3.5 MiB (512 * 512 * 2 bytes * 7), giving the cache
 * room to retain both current and incoming cards during a carousel transition.
 * The artwork for all games remains in the program's source data; only the
 * current viewport and its transition lead-in are resident in VRAM.
 *
 * Every VRAM allocation and descriptor is created once at browser init. While
 * browsing, cache replacement only overwrites a slot that is outside the
 * current visible range, after the previous ta_render() has completed.
 */

static char g_rom_names[MAX_ROMS][ROM_NAME_LEN];
static int  g_rom_count = 0;

static font_t *font_18pt = NULL;
static font_t *font_14pt = NULL;

static texture_description_t *bg_tex = NULL;
static texture_description_t *logo_tex = NULL;
static texture_description_t *joy1_tex = NULL;
static texture_description_t *joy2_tex = NULL;
static texture_description_t *joy3_tex = NULL;
static texture_description_t *btn_main_tex = NULL;

/* Background music player. The audio ringbuffer is owned by main.c. */
static xmp_context g_xmp_ctx = NULL;
static int g_music_initialized = 0;
static int g_music_speed_applied = 0;

/* Decoded menu-highlight OGG, stored as interleaved 44.1 kHz stereo PCM. */
static int16_t *g_menu_sfx_pcm = NULL;
static uint32_t g_menu_sfx_frames = 0;
static uint32_t g_menu_sfx_voice_pos[MENU_SFX_VOICE_COUNT];
static unsigned int g_menu_sfx_next_voice = 0;

/* A separate copy is needed only while an effect is being mixed. It remains
 * valid if audio_write_stereo_data() accepts only part of the tracker frame. */
static int16_t g_music_mix_buffer[MUSIC_MIX_BUFFER_BYTES / sizeof(int16_t)]
    __attribute__((aligned(32)));

/* Fractional 60 Hz display-frame accumulator for smooth tracker pacing. */
static uint64_t g_music_fallback_phase = 0;
static uint32_t g_music_fallback_frame_time = 20000u;

/*
 * Keep any unqueued part of the current libxmp frame.
 * audio_write_stereo_data() may accept fewer samples than requested when the
 * AICA ringbuffer has insufficient free space; the remainder must not be
 * discarded or the music becomes broken/distorted. The libxmp buffer remains
 * valid until the next xmp_play_frame(), so do not advance while this is set.
 */
static const uint8_t *g_music_pending_buffer = NULL;
static unsigned int g_music_pending_samples = 0;
static unsigned int g_music_pending_offset = 0;

typedef struct {
    void *vram;
    texture_description_t *desc;
    int rom_index;
    uint32_t last_used;
    unsigned int source_width;
    unsigned int source_height;
} cover_cache_t;

static cover_cache_t g_cover_cache[COVER_CACHE_SIZE];

/* Shared staging buffer: one cover is converted/uploaded at a time. */
static uint16_t g_cover_upload_pixels[COVER_TEXTURE_PIXELS]
    __attribute__((aligned(32)));

static float current_scroll = 0.0f;
static uint32_t frame_counter = 0;
static int g_romfs_initialized = 0;
static int g_romfs_scan_failed = 0;
static int g_scroll_initialized = 0;
static int g_last_requested_selected = -1;
static int g_priority_cover_index = -1;

/* One pending texture upload is spread over several browser frames. */
static int g_upload_active = 0;
static int g_upload_slot = -1;
static int g_upload_rom_index = -1;
static const uint16_t *g_upload_pixels = NULL;
static unsigned int g_upload_next_y = 0;
static unsigned int g_upload_source_width = 0;
static unsigned int g_upload_source_height = 0;

extern uint8_t *background_png_data;
extern unsigned int background_png_width;
extern unsigned int background_png_height;

extern uint8_t *logo_png_data;
extern unsigned int logo_png_width;
extern unsigned int logo_png_height;

extern uint8_t *joy1_png_data;
extern unsigned int joy1_png_width;
extern unsigned int joy1_png_height;

extern uint8_t *joy2_png_data;
extern unsigned int joy2_png_width;
extern unsigned int joy2_png_height;

extern uint8_t *joy3_png_data;
extern unsigned int joy3_png_width;
extern unsigned int joy3_png_height;

extern uint8_t *btn_main_png_data;
extern unsigned int btn_main_png_width;
extern unsigned int btn_main_png_height;

extern uint8_t *cover_mario_png_data;
extern unsigned int cover_mario_png_width;
extern unsigned int cover_mario_png_height;

extern uint8_t *cover_zelda_png_data;
extern unsigned int cover_zelda_png_width;
extern unsigned int cover_zelda_png_height;

extern uint8_t *cover_smb3_png_data;
extern unsigned int cover_smb3_png_width;
extern unsigned int cover_smb3_png_height;

extern uint8_t *cover_castlevania3_png_data;
extern unsigned int cover_castlevania3_png_width;
extern unsigned int cover_castlevania3_png_height;

extern uint8_t *cover_duckhunt_png_data;
extern unsigned int cover_duckhunt_png_width;
extern unsigned int cover_duckhunt_png_height;

extern uint8_t *cover_excitebike_png_data;
extern unsigned int cover_excitebike_png_width;
extern unsigned int cover_excitebike_png_height;

extern uint8_t *cover_gradius_png_data;
extern unsigned int cover_gradius_png_width;
extern unsigned int cover_gradius_png_height;

extern uint8_t *cover_castlevania_png_data;
extern unsigned int cover_castlevania_png_width;
extern unsigned int cover_castlevania_png_height;

extern uint8_t *cover_kirby_png_data;
extern unsigned int cover_kirby_png_width;
extern unsigned int cover_kirby_png_height;

extern uint8_t *cover_megaman2_png_data;
extern unsigned int cover_megaman2_png_width;
extern unsigned int cover_megaman2_png_height;

extern uint8_t *cover_metroid_png_data;
extern unsigned int cover_metroid_png_width;
extern unsigned int cover_metroid_png_height;

extern uint8_t *cover_prince_png_data;
extern unsigned int cover_prince_png_width;
extern unsigned int cover_prince_png_height;

extern uint8_t *cover_punchout_png_data;
extern unsigned int cover_punchout_png_width;
extern unsigned int cover_punchout_png_height;

extern uint8_t *cover_streemerz_png_data;
extern unsigned int cover_streemerz_png_width;
extern unsigned int cover_streemerz_png_height;

extern uint8_t *cover_crystalmines_png_data;
extern unsigned int cover_crystalmines_png_width;
extern unsigned int cover_crystalmines_png_height;

extern uint8_t *cover_gargoyle2_png_data;
extern unsigned int cover_gargoyle2_png_width;
extern unsigned int cover_gargoyle2_png_height;

extern uint8_t *cover_marble_png_data;
extern unsigned int cover_marble_png_width;
extern unsigned int cover_marble_png_height;

extern uint8_t *cover_crazyclimber_png_data;
extern unsigned int cover_crazyclimber_png_width;
extern unsigned int cover_crazyclimber_png_height;

extern uint8_t *cover_deadlytowers_png_data;
extern unsigned int cover_deadlytowers_png_width;
extern unsigned int cover_deadlytowers_png_height;

extern uint8_t *cover_holydiver_png_data;
extern unsigned int cover_holydiver_png_width;
extern unsigned int cover_holydiver_png_height;

extern uint8_t *cover_littlecisyphus_png_data;
extern unsigned int cover_littlecisyphus_png_width;
extern unsigned int cover_littlecisyphus_png_height;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Positive modulo: unlike C's %, this always returns an index in [0,count). */
static int wrap_index(int index)
{
    if (g_rom_count <= 0) {
        return -1;
    }

    index %= g_rom_count;
    if (index < 0) {
        index += g_rom_count;
    }

    return index;
}

/* Return the shortest signed carousel distance from current_scroll to selected. */
static float shortest_scroll_diff(int selected)
{
    float diff;
    float half_count;

    if (g_rom_count <= 1) {
        return 0.0f;
    }

    diff = (float)selected - current_scroll;
    half_count = (float)g_rom_count * 0.5f;

    if (diff > half_count) {
        diff -= (float)g_rom_count;
    }
    if (diff < -half_count) {
        diff += (float)g_rom_count;
    }

    return diff;
}

static int is_supported_texture_size(unsigned int size)
{
    switch (size) {
        case 8:
        case 16:
        case 32:
        case 64:
        case 128:
        case 256:
        case 512:
        case 1024:
            return 1;
        default:
            return 0;
    }
}

/* ta_texture_desc_malloc_direct() creates a square texture from one UV size. */
static texture_description_t *load_static_texture(uint8_t *data,
                                                  unsigned int width,
                                                  unsigned int height)
{
    if (!data || width != height || !is_supported_texture_size(width)) {
        return NULL;
    }

    return ta_texture_desc_malloc_direct(
        (int)width,
        data,
        TA_TEXTUREMODE_ARGB4444
    );
}

static int has_nes_extension(const char *name, size_t length)
{
    const char *extension;

    if (!name || length <= 4u) {
        return 0;
    }

    extension = name + length - 4u;
    return extension[0] == '.' &&
           tolower((unsigned char)extension[1]) == 'n' &&
           tolower((unsigned char)extension[2]) == 'e' &&
           tolower((unsigned char)extension[3]) == 's';
}

static void scan_romfs(void)
{
    DIR *dir = opendir("rom://");
    struct dirent *entry;

    g_rom_count = 0;
    g_romfs_scan_failed = 0;

    if (!dir) {
        g_romfs_scan_failed = 1;
        return;
    }

    while ((entry = readdir(dir)) != NULL && g_rom_count < MAX_ROMS) {
        size_t len = strlen(entry->d_name);

        if (!has_nes_extension(entry->d_name, len)) {
            continue;
        }

        /* Do not store a truncated name: main.c must open the exact ROM path. */
        if (len >= ROM_NAME_LEN) {
            continue;
        }

        memcpy(g_rom_names[g_rom_count], entry->d_name, len + 1u);
        g_rom_count++;
    }

    closedir(dir);
}

/* ------------------------------------------------------------------------- */
/* Cover artwork lookup                                                      */
/* ------------------------------------------------------------------------- */

static void get_cover_data(int index, uint8_t **data,
                           unsigned int *w, unsigned int *h)
{
    *data = NULL;
    *w = 0;
    *h = 0;

    if (index < 0 || index >= g_rom_count) {
        return;
    }

    const char *name = g_rom_names[index];

    if (strstr(name, "Castlevania III") != NULL || strstr(name, "Dracula") != NULL) {
        *data = cover_castlevania3_png_data;
        *w = cover_castlevania3_png_width;
        *h = cover_castlevania3_png_height;
    }
    else if (strstr(name, "Castlevania") != NULL) {
        *data = cover_castlevania_png_data;
        *w = cover_castlevania_png_width;
        *h = cover_castlevania_png_height;
    }
    else if (strstr(name, "Bros. 3") != NULL || strstr(name, "Bros 3") != NULL) {
        *data = cover_smb3_png_data;
        *w = cover_smb3_png_width;
        *h = cover_smb3_png_height;
    }
    else if (strstr(name, "Mario") != NULL) {
        *data = cover_mario_png_data;
        *w = cover_mario_png_width;
        *h = cover_mario_png_height;
    }
    else if (strstr(name, "Zelda") != NULL) {
        *data = cover_zelda_png_data;
        *w = cover_zelda_png_width;
        *h = cover_zelda_png_height;
    }
    else if (strstr(name, "Duck") != NULL) {
        *data = cover_duckhunt_png_data;
        *w = cover_duckhunt_png_width;
        *h = cover_duckhunt_png_height;
    }
    else if (strstr(name, "Excite") != NULL) {
        *data = cover_excitebike_png_data;
        *w = cover_excitebike_png_width;
        *h = cover_excitebike_png_height;
    }
    else if (strstr(name, "Gradius") != NULL) {
        *data = cover_gradius_png_data;
        *w = cover_gradius_png_width;
        *h = cover_gradius_png_height;
    }
    else if (strstr(name, "Kirby") != NULL) {
        *data = cover_kirby_png_data;
        *w = cover_kirby_png_width;
        *h = cover_kirby_png_height;
    }
    else if (strstr(name, "Mega Man 2") != NULL || strstr(name, "MegaMan 2") != NULL) {
        *data = cover_megaman2_png_data;
        *w = cover_megaman2_png_width;
        *h = cover_megaman2_png_height;
    }
    else if (strstr(name, "Metroid") != NULL) {
        *data = cover_metroid_png_data;
        *w = cover_metroid_png_width;
        *h = cover_metroid_png_height;
    }
    else if (strstr(name, "Prince") != NULL || strstr(name, "Persia") != NULL) {
        *data = cover_prince_png_data;
        *w = cover_prince_png_width;
        *h = cover_prince_png_height;
    }
    else if (strstr(name, "Punch") != NULL || strstr(name, "Tyson") != NULL ||
             strstr(name, "Mike") != NULL || strstr(name, "Boxing") != NULL) {
        *data = cover_punchout_png_data;
        *w = cover_punchout_png_width;
        *h = cover_punchout_png_height;
    }
    else if (strstr(name, "Streemerz") != NULL) {
        *data = cover_streemerz_png_data;
        *w = cover_streemerz_png_width;
        *h = cover_streemerz_png_height;
    }
    else if (strstr(name, "Crystal") != NULL) {
        *data = cover_crystalmines_png_data;
        *w = cover_crystalmines_png_width;
        *h = cover_crystalmines_png_height;
    }
    else if (strstr(name, "Gargoyle") != NULL) {
        *data = cover_gargoyle2_png_data;
        *w = cover_gargoyle2_png_width;
        *h = cover_gargoyle2_png_height;
    }
    else if (strstr(name, "Marble") != NULL) {
        *data = cover_marble_png_data;
        *w = cover_marble_png_width;
        *h = cover_marble_png_height;
    }
    else if (strstr(name, "Crazy Climber") != NULL ||
             strstr(name, "CrazyClimber") != NULL) {
        *data = cover_crazyclimber_png_data;
        *w = cover_crazyclimber_png_width;
        *h = cover_crazyclimber_png_height;
    }
    else if (strstr(name, "Deadly Towers") != NULL ||
             strstr(name, "DeadlyTowers") != NULL) {
        *data = cover_deadlytowers_png_data;
        *w = cover_deadlytowers_png_width;
        *h = cover_deadlytowers_png_height;
    }
    else if (strstr(name, "Holy Diver") != NULL ||
             strstr(name, "HolyDiver") != NULL) {
        *data = cover_holydiver_png_data;
        *w = cover_holydiver_png_width;
        *h = cover_holydiver_png_height;
    }
    else if (strstr(name, "Little Cisyphus") != NULL ||
             strstr(name, "LittleCisyphus") != NULL ||
             strstr(name, "Little Sisyphus") != NULL ||
             strstr(name, "LittleSisyphus") != NULL) {
        *data = cover_littlecisyphus_png_data;
        *w = cover_littlecisyphus_png_width;
        *h = cover_littlecisyphus_png_height;
    }
    else {
        /* Keep the original default artwork for games without a specific map. */
        *data = cover_mario_png_data;
        *w = cover_mario_png_width;
        *h = cover_mario_png_height;
    }
}

/* ------------------------------------------------------------------------- */
/* Cover texture cache                                                       */
/* ------------------------------------------------------------------------- */

static int is_cover_loaded(int index)
{
    int i;

    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        if (g_cover_cache[i].rom_index == index && g_cover_cache[i].desc != NULL) {
            return 1;
        }
    }

    return 0;
}

/* Return whether an index is drawn at one particular scroll position. */
static int is_cover_visible_at_scroll(int index, float scroll)
{
    int base_idx;
    float offset;
    int i;

    if (g_rom_count <= 0 || index < 0 || index >= g_rom_count) {
        return 0;
    }

    base_idx = (int)floorf(scroll);
    offset = scroll - (float)base_idx;

    for (i = -3; i <= 3; i++) {
        if (fabsf((float)i - offset) <= 2.5f &&
            wrap_index(base_idx + i) == index) {
            return 1;
        }
    }

    return 0;
}

/*
 * Protect the union of cards visible between the current scroll position and
 * the selected destination. This matters because a card can be just outside
 * the viewport now but enter it a few animation frames later.
 */
static int is_cover_needed_during_transition(int index, int selected)
{
    const int samples = 16;
    float diff;
    int step;

    if (g_rom_count <= 0 || index < 0 || index >= g_rom_count) {
        return 0;
    }

    diff = shortest_scroll_diff(selected);
    for (step = 0; step <= samples; step++) {
        float t = (float)step / (float)samples;
        float position = current_scroll + (diff * t);

        if (is_cover_visible_at_scroll(index, position)) {
            return 1;
        }
    }

    return 0;
}

static void release_cover_slot(int slot)
{
    if (slot < 0 || slot >= COVER_CACHE_SIZE) {
        return;
    }

    /* Descriptors created with ta_texture_desc_direct() do not own VRAM. */
    if (g_cover_cache[slot].desc) {
        ta_texture_desc_free(g_cover_cache[slot].desc);
        g_cover_cache[slot].desc = NULL;
    }

    if (g_cover_cache[slot].vram) {
        ta_texture_free(g_cover_cache[slot].vram);
        g_cover_cache[slot].vram = NULL;
    }

    g_cover_cache[slot].rom_index = -1;
    g_cover_cache[slot].last_used = 0;
    g_cover_cache[slot].source_width = 0;
    g_cover_cache[slot].source_height = 0;
}

static void mark_cover_slot_empty(int slot)
{
    if (slot < 0 || slot >= COVER_CACHE_SIZE) {
        return;
    }

    /* Keep the VRAM allocation and descriptor; only invalidate its contents. */
    g_cover_cache[slot].rom_index = -1;
    g_cover_cache[slot].last_used = 0;
    g_cover_cache[slot].source_width = 0;
    g_cover_cache[slot].source_height = 0;
}

/*
 * Return the pixels to upload into one 512x512 slot. Native 512x512 assets
 * are passed through unchanged, preserving every source pixel. Other supported
 * square sizes are resampled into the staging buffer so the descriptor remains
 * a fixed 512x512 texture.
 */
static const uint16_t *prepare_cover_pixels(const uint8_t *data,
                                            unsigned int source_width,
                                            unsigned int source_height)
{
    const uint16_t *source = (const uint16_t *)data;
    unsigned int y;

    if (!data || source_width == 0 || source_height == 0) {
        return NULL;
    }

    if (source_width == COVER_TEXTURE_SIZE &&
        source_height == COVER_TEXTURE_SIZE) {
        return source;
    }

    for (y = 0; y < COVER_TEXTURE_SIZE; y++) {
        unsigned int source_y =
            (unsigned int)(((uint32_t)y * source_height) / COVER_TEXTURE_SIZE);
        unsigned int x;

        if (source_y >= source_height) {
            source_y = source_height - 1;
        }

        for (x = 0; x < COVER_TEXTURE_SIZE; x++) {
            unsigned int source_x =
                (unsigned int)(((uint32_t)x * source_width) / COVER_TEXTURE_SIZE);

            if (source_x >= source_width) {
                source_x = source_width - 1;
            }

            g_cover_upload_pixels[y * COVER_TEXTURE_SIZE + x] =
                source[source_y * source_width + source_x];
        }
    }

    return g_cover_upload_pixels;
}

static void release_static_texture(texture_description_t **texture)
{
    if (texture && *texture) {
        /* These descriptors were created with ta_texture_desc_malloc_direct(),
         * so ta_texture_desc_free() also releases their VRAM allocation. */
        ta_texture_desc_free(*texture);
        *texture = NULL;
    }
}

static int find_cover_slot(int protected_index, int selected)
{
    int slot = -1;
    uint32_t oldest_age = 0;
    int i;

    /* Prefer an allocated but currently unused slot. */
    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        if (g_cover_cache[i].vram && g_cover_cache[i].desc &&
            g_cover_cache[i].rom_index < 0) {
            return i;
        }
    }

    /* Never overwrite a cover needed anywhere along the current transition. */
    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        int cached_index = g_cover_cache[i].rom_index;
        uint32_t age;

        if (!g_cover_cache[i].vram || !g_cover_cache[i].desc) {
            continue;
        }
        if (cached_index == protected_index ||
            is_cover_needed_during_transition(cached_index, selected)) {
            continue;
        }

        age = frame_counter - g_cover_cache[i].last_used;
        if (slot < 0 || age > oldest_age) {
            oldest_age = age;
            slot = i;
        }
    }

    return slot;
}

static int load_cover_direct(int index, int protected_index)
{
    uint8_t *data;
    unsigned int w;
    unsigned int h;
    int slot;
    const uint16_t *upload_pixels;

    if (index < 0 || index >= g_rom_count) {
        return 0;
    }

    if (is_cover_loaded(index)) {
        return 1;
    }

    get_cover_data(index, &data, &w, &h);

    /* The artwork supplied by the current browser is square RGBA4444 data. */
    if (!data || w != h || !is_supported_texture_size(w)) {
        return 0;
    }

    slot = find_cover_slot(protected_index, protected_index);
    if (slot < 0) {
        return 0;
    }

    /* The old image is no longer visible, so this slot may be overwritten. */
    mark_cover_slot_empty(slot);

    upload_pixels = prepare_cover_pixels(data, w, h);
    if (!upload_pixels) {
        return 0;
    }

    /*
     * The slot's VRAM and descriptor are persistent. No allocator operation
     * occurs while browsing; only the pixel contents of this non-visible slot
     * are updated after the previous ta_render() has completed. Native 512x512
     * artwork reaches ta_texture_load() unchanged.
     */
    if (ta_texture_load(g_cover_cache[slot].vram,
                        COVER_TEXTURE_SIZE,
                        16,
                        (void *)upload_pixels) != 0) {
        return 0;
    }

    g_cover_cache[slot].rom_index = index;
    g_cover_cache[slot].last_used = frame_counter;
    g_cover_cache[slot].source_width = w;
    g_cover_cache[slot].source_height = h;

    return 1;
}

/* Start a new upload without touching a texture that is visible in the
 * current-to-selected animation path. The slot becomes visible in the cache
 * only after all 512 rows have been uploaded. */
static int start_cover_upload(int index, int selected)
{
    uint8_t *data;
    unsigned int w;
    unsigned int h;
    int slot;
    const uint16_t *pixels;

    if (index < 0 || index >= g_rom_count || g_upload_active) {
        return 0;
    }
    if (is_cover_loaded(index)) {
        return 1;
    }

    get_cover_data(index, &data, &w, &h);
    if (!data || w != h || !is_supported_texture_size(w)) {
        return 0;
    }

    slot = find_cover_slot(index, selected);
    if (slot < 0) {
        return 0;
    }

    /* Build/resample the source once. The staging buffer remains untouched
     * until this single pending upload has completed. */
    pixels = prepare_cover_pixels(data, w, h);
    if (!pixels) {
        return 0;
    }

    mark_cover_slot_empty(slot);
    g_upload_active = 1;
    g_upload_slot = slot;
    g_upload_rom_index = index;
    g_upload_pixels = pixels;
    g_upload_next_y = 0;
    g_upload_source_width = w;
    g_upload_source_height = h;
    return 1;
}

/* Upload only a horizontal band per frame. ta_texture_load_sprite() writes
 * that band into its final twiddled 512x512 location, avoiding one long
 * full-cover conversion/upload in the middle of the carousel animation. */
static void advance_cover_upload(void)
{
    unsigned int rows;
    int result;

    if (!g_upload_active || g_upload_slot < 0 ||
        g_upload_slot >= COVER_CACHE_SIZE || !g_upload_pixels) {
        return;
    }

    rows = COVER_TEXTURE_SIZE - g_upload_next_y;
    if (rows > COVER_UPLOAD_ROWS_PER_FRAME) {
        rows = COVER_UPLOAD_ROWS_PER_FRAME;
    }

    result = ta_texture_load_sprite(
        g_cover_cache[g_upload_slot].vram,
        COVER_TEXTURE_SIZE,
        16,
        0,
        (int)g_upload_next_y,
        COVER_TEXTURE_SIZE,
        (int)rows,
        (void *)(g_upload_pixels + (g_upload_next_y * COVER_TEXTURE_SIZE))
    );

    if (result != 0) {
        mark_cover_slot_empty(g_upload_slot);
        if (g_priority_cover_index == g_upload_rom_index) {
            g_priority_cover_index = -1;
        }
        g_upload_active = 0;
        g_upload_slot = -1;
        g_upload_rom_index = -1;
        g_upload_pixels = NULL;
        g_upload_next_y = 0;
        g_upload_source_width = 0;
        g_upload_source_height = 0;
        return;
    }

    g_upload_next_y += rows;
    if (g_upload_next_y >= COVER_TEXTURE_SIZE) {
        cover_cache_t *slot = &g_cover_cache[g_upload_slot];

        slot->rom_index = g_upload_rom_index;
        slot->last_used = frame_counter;
        slot->source_width = g_upload_source_width;
        slot->source_height = g_upload_source_height;

        if (g_priority_cover_index == g_upload_rom_index) {
            g_priority_cover_index = -1;
        }

        g_upload_active = 0;
        g_upload_slot = -1;
        g_upload_rom_index = -1;
        g_upload_pixels = NULL;
        g_upload_next_y = 0;
    }
}

static cover_cache_t *get_cover_slot_for_index(int index)
{
    int i;

    if (index < 0 || index >= g_rom_count) {
        return NULL;
    }

    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        if (g_cover_cache[i].rom_index == index && g_cover_cache[i].desc) {
            g_cover_cache[i].last_used = frame_counter;
            return &g_cover_cache[i];
        }
    }

    return NULL;
}

/*
 * Run once per browser frame.
 *
 * When the user changes selection, queue the new cover that will enter from
 * the leading edge of the carousel (selected +/- 2). Its texture is uploaded
 * in 64-row bands. For a one-step move, eight small uploads finish before that
 * card reaches the renderer's +/-2.5 visibility boundary.
 *
 * The currently visible viewport is also checked as a fallback for startup,
 * large selection jumps, or a cache slot that was unavailable earlier.
 */
static void prepare_cover_cache(int selected)
{
    int base_idx;
    float offset;
    float best_distance = 1000000.0f;
    int best_index = -1;
    int i;

    if (g_rom_count <= 0) {
        return;
    }

    /* Never begin a second upload while the current slot is only partially
     * populated. This also keeps the shared staging buffer stable. */
    if (g_upload_active) {
        advance_cover_upload();
        return;
    }

    if (g_priority_cover_index >= 0 &&
        is_cover_loaded(g_priority_cover_index)) {
        g_priority_cover_index = -1;
    }

    /* Preload the future edge card immediately after a selection change,
     * rather than waiting until it becomes visible halfway through motion. */
    if (g_priority_cover_index >= 0) {
        if (start_cover_upload(g_priority_cover_index, selected)) {
            advance_cover_upload();
            return;
        }
    }

    base_idx = (int)floorf(current_scroll);
    offset = current_scroll - (float)base_idx;

    /* Fallback: find the nearest missing card in the current display list. */
    for (i = -3; i <= 3; i++) {
        float virtual_i = (float)i - offset;
        int index;
        float distance;

        if (fabsf(virtual_i) > 2.5f) {
            continue;
        }

        index = wrap_index(base_idx + i);
        if (index < 0 || is_cover_loaded(index)) {
            continue;
        }

        distance = fabsf(virtual_i);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = index;
        }
    }

    if (best_index >= 0 && start_cover_upload(best_index, selected)) {
        advance_cover_upload();
    }
}

/* ------------------------------------------------------------------------- */
/* Background music                                                          */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* OGG menu selection sound                                                  */
/* ------------------------------------------------------------------------- */

static void release_menu_select_sfx(void)
{
    free(g_menu_sfx_pcm);
    g_menu_sfx_pcm = NULL;
    g_menu_sfx_frames = 0;
    memset(g_menu_sfx_voice_pos, 0, sizeof(g_menu_sfx_voice_pos));
    g_menu_sfx_next_voice = 0;
}

/*
 * Decode the short OGG effect at menu startup. libvorbisfile is part of the
 * libnaomi 3rdparty ports (libvorbisfile -> libvorbis -> libogg). The decoded
 * data is kept in RAM so selection changes never perform file I/O or Vorbis
 * decoding in the frame loop.
 *
 * Output is normalized to interleaved signed 16-bit stereo at the same 44.1
 * kHz rate as main.c's existing AICA ringbuffer. Mono source files are copied
 * to both channels; non-44.1 kHz files are linearly resampled once at startup.
 */
static int load_menu_select_sfx(void)
{
    FILE *fp = NULL;
    OggVorbis_File ogg;
    vorbis_info *info;
    int ogg_open = 0;
    int channels;
    uint32_t source_rate;
    uint64_t total_frames;
    uint64_t source_bytes_64;
    size_t source_bytes;
    size_t decoded_bytes = 0;
    size_t bytes_per_frame;
    int16_t *source_pcm = NULL;
    int16_t *output_pcm = NULL;
    uint64_t source_frames;
    uint64_t output_frames;
    uint64_t output_bytes_64;
    int result = 0;

    if (g_menu_sfx_pcm && g_menu_sfx_frames > 0) {
        return 1;
    }

    fp = fopen(MENU_SFX_FILE_PATH, "rb");
    if (!fp) {
        return 0;
    }

    if (ov_open(fp, &ogg, NULL, 0) < 0) {
        /* ov_open() does not take ownership of fp when it fails. */
        fclose(fp);
        return 0;
    }
    ogg_open = 1;

    info = ov_info(&ogg, -1);
    if (!info || (info->channels != 1 && info->channels != 2) ||
        info->rate < 8000 || info->rate > 192000) {
        goto cleanup;
    }

    channels = info->channels;
    source_rate = (uint32_t)info->rate;
    total_frames = (uint64_t)ov_pcm_total(&ogg, -1);

    /* A menu click should be short. This also bounds memory on malformed files. */
    if (total_frames == 0 ||
        total_frames > (uint64_t)source_rate * MENU_SFX_MAX_SECONDS) {
        goto cleanup;
    }

    bytes_per_frame = (size_t)channels * sizeof(int16_t);
    source_bytes_64 = total_frames * (uint64_t)bytes_per_frame;
    if (source_bytes_64 == 0 || source_bytes_64 > (uint64_t)SIZE_MAX) {
        goto cleanup;
    }
    source_bytes = (size_t)source_bytes_64;

    source_pcm = (int16_t *)malloc(source_bytes);
    if (!source_pcm) {
        goto cleanup;
    }

    while (decoded_bytes < source_bytes) {
        size_t remaining = source_bytes - decoded_bytes;
        int request = (remaining > 4096u) ? 4096 : (int)remaining;
        int bitstream = 0;
        long got;
        vorbis_info *current_info;

        request -= request % (int)bytes_per_frame;
        if (request <= 0) {
            break;
        }

        got = ov_read(&ogg,
                      (char *)source_pcm + decoded_bytes,
                      request,
                      0,      /* little endian */
                      2,      /* 16-bit output */
                      1,      /* signed output */
                      &bitstream);
        if (got == 0) {
            break;
        }
        if (got < 0 || (size_t)got > remaining ||
            ((size_t)got % bytes_per_frame) != 0) {
            goto cleanup;
        }

        /* Reject chained streams whose sample format changes mid-file. */
        current_info = ov_info(&ogg, bitstream);
        if (!current_info || current_info->channels != channels ||
            current_info->rate != (long)source_rate) {
            goto cleanup;
        }

        decoded_bytes += (size_t)got;
    }

    if (decoded_bytes != source_bytes ||
        (decoded_bytes % bytes_per_frame) != 0) {
        goto cleanup;
    }

    source_frames = (uint64_t)(decoded_bytes / bytes_per_frame);
    output_frames =
        (source_frames * MENU_SFX_OUTPUT_RATE + (source_rate / 2u)) /
        source_rate;
    if (output_frames == 0 ||
        output_frames > (uint64_t)MENU_SFX_OUTPUT_RATE * MENU_SFX_MAX_SECONDS) {
        goto cleanup;
    }

    output_bytes_64 = output_frames * 2u * sizeof(int16_t);
    if (output_bytes_64 > (uint64_t)SIZE_MAX) {
        goto cleanup;
    }

    output_pcm = (int16_t *)malloc((size_t)output_bytes_64);
    if (!output_pcm) {
        goto cleanup;
    }

    /* Linear resampling is done once here, never during carousel navigation. */
    for (uint64_t out_frame = 0; out_frame < output_frames; out_frame++) {
        uint64_t source_position = out_frame * source_rate;
        uint64_t source_index = source_position / MENU_SFX_OUTPUT_RATE;
        uint32_t fraction = (uint32_t)(source_position % MENU_SFX_OUTPUT_RATE);
        uint64_t next_index;
        int32_t left0;
        int32_t left1;
        int32_t right0;
        int32_t right1;
        int32_t left;
        int32_t right;

        if (source_index >= source_frames) {
            source_index = source_frames - 1u;
        }
        next_index = (source_index + 1u < source_frames)
            ? source_index + 1u
            : source_index;

        if (channels == 1) {
            left0 = source_pcm[source_index];
            left1 = source_pcm[next_index];
            right0 = left0;
            right1 = left1;
        } else {
            left0 = source_pcm[source_index * 2u];
            left1 = source_pcm[next_index * 2u];
            right0 = source_pcm[source_index * 2u + 1u];
            right1 = source_pcm[next_index * 2u + 1u];
        }

        left = (int32_t)(((int64_t)left0 *
                         (MENU_SFX_OUTPUT_RATE - fraction) +
                         (int64_t)left1 * fraction) /
                        MENU_SFX_OUTPUT_RATE);
        right = (int32_t)(((int64_t)right0 *
                          (MENU_SFX_OUTPUT_RATE - fraction) +
                          (int64_t)right1 * fraction) /
                         MENU_SFX_OUTPUT_RATE);

        output_pcm[out_frame * 2u] = (int16_t)left;
        output_pcm[out_frame * 2u + 1u] = (int16_t)right;
    }

    g_menu_sfx_pcm = output_pcm;
    g_menu_sfx_frames = (uint32_t)output_frames;
    output_pcm = NULL;
    result = 1;

cleanup:
    free(source_pcm);
    free(output_pcm);
    if (ogg_open) {
        ov_clear(&ogg);
    }
    return result;
}

static void play_menu_select_sfx(void)
{
    unsigned int n;
    unsigned int voice = MENU_SFX_VOICE_COUNT;

    if (!g_menu_sfx_pcm || g_menu_sfx_frames == 0) {
        return;
    }

    /* Prefer an idle voice. If all are busy, replace the next voice so fast
     * repeated button presses can still be heard without allocating memory. */
    for (n = 0; n < MENU_SFX_VOICE_COUNT; n++) {
        unsigned int candidate =
            (g_menu_sfx_next_voice + n) % MENU_SFX_VOICE_COUNT;
        if (g_menu_sfx_voice_pos[candidate] == 0) {
            voice = candidate;
            break;
        }
    }
    if (voice == MENU_SFX_VOICE_COUNT) {
        voice = g_menu_sfx_next_voice;
    }

    /* Position is stored as sample-frame index + 1; zero means inactive. */
    g_menu_sfx_voice_pos[voice] = 1;
    g_menu_sfx_next_voice = (voice + 1u) % MENU_SFX_VOICE_COUNT;
}

static int menu_select_sfx_has_active_voice(void)
{
    unsigned int i;

    for (i = 0; i < MENU_SFX_VOICE_COUNT; i++) {
        if (g_menu_sfx_voice_pos[i] != 0) {
            return 1;
        }
    }
    return 0;
}

static int16_t clamp_sfx_mix_sample(int32_t sample)
{
    if (sample > 32767) {
        sample = 32767;
    } else if (sample < -32768) {
        sample = -32768;
    }
    return (int16_t)sample;
}

/* Add active OGG voices to one interleaved 16-bit stereo tracker frame. */
static void mix_menu_select_sfx(int16_t *stereo, unsigned int frame_count)
{
    unsigned int frame;

    if (!stereo || !g_menu_sfx_pcm || g_menu_sfx_frames == 0) {
        return;
    }

    for (frame = 0; frame < frame_count; frame++) {
        int32_t left = stereo[frame * 2u];
        int32_t right = stereo[frame * 2u + 1u];
        unsigned int voice;

        for (voice = 0; voice < MENU_SFX_VOICE_COUNT; voice++) {
            uint32_t encoded_position = g_menu_sfx_voice_pos[voice];
            uint32_t sample_frame;

            if (encoded_position == 0) {
                continue;
            }

            sample_frame = encoded_position - 1u;
            if (sample_frame >= g_menu_sfx_frames) {
                g_menu_sfx_voice_pos[voice] = 0;
                continue;
            }

            left += ((int32_t)g_menu_sfx_pcm[sample_frame * 2u] *
                     MENU_SFX_GAIN_Q15) >> 15;
            right += ((int32_t)g_menu_sfx_pcm[sample_frame * 2u + 1u] *
                      MENU_SFX_GAIN_Q15) >> 15;

            sample_frame++;
            g_menu_sfx_voice_pos[voice] =
                (sample_frame < g_menu_sfx_frames) ? sample_frame + 1u : 0;
        }

        stereo[frame * 2u] = clamp_sfx_mix_sample(left);
        stereo[frame * 2u + 1u] = clamp_sfx_mix_sample(right);
    }
}

static void init_background_music(void)
{
    if (g_music_initialized) {
        return;
    }

    /*
     * main.c already calls audio_init() and registers the 44.1 kHz, 16-bit
     * stereo ringbuffer before rom_browser_init(). Do not initialize audio or
     * register another ringbuffer here: audio_register_ringbuffer() replaces
     * the existing one and resets its playback/write positions.
     */
    g_xmp_ctx = xmp_create_context();
    if (!g_xmp_ctx) {
        return;
    }

    if (xmp_load_module(g_xmp_ctx, MUSIC_FILE_PATH) < 0) {
        xmp_free_context(g_xmp_ctx);
        g_xmp_ctx = NULL;
        return;
    }

    /* Decode the OGG once. If it is absent or invalid, keep the music alive. */
    (void)load_menu_select_sfx();

    /*
     * Format 0 is libxmp's default signed 16-bit stereo output. This matches
     * main.c's AUDIO_FORMAT_16BIT / 44100 Hz ringbuffer.
     */
    if (xmp_start_player(g_xmp_ctx, 44100, 0) < 0) {
        release_menu_select_sfx();
        xmp_release_module(g_xmp_ctx);
        xmp_free_context(g_xmp_ctx);
        g_xmp_ctx = NULL;
        return;
    }

    /* Cubic interpolation; AMP=1 is libxmp's normal default level. */
    (void)xmp_set_player(g_xmp_ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
    (void)xmp_set_player(g_xmp_ctx, XMP_PLAYER_AMP, 1);

    /*
     * Use xmp_set_tempo_factor() for compatibility with the older libxmp
     * version bundled with the Naomi build. For this XM module the default
     * base tempo factor is 1.0, so 100/110 shortens tracker ticks to give
     * 10% faster playback without changing the 44.1 kHz sample rate or pitch.
     * libxmp does not update fi.frame_time for this setting; the scheduler
     * below applies the same factor when calculating its frame cadence.
     */
    g_music_speed_applied =
        (xmp_set_tempo_factor(
            g_xmp_ctx, 100.0 / (double)MUSIC_SPEED_PERCENT) == 0);

    /* Restore full ringbuffer output level; 0.7 was unnecessarily attenuating. */
    (void)audio_change_ringbuffer_volume(1.0f);

    g_music_pending_buffer = NULL;
    g_music_pending_samples = 0;
    g_music_pending_offset = 0;
    g_music_fallback_phase = 0;
    g_music_fallback_frame_time = (uint32_t)(
        (20000ULL * 100ULL + (MUSIC_SPEED_PERCENT / 2u)) /
        (g_music_speed_applied ? MUSIC_SPEED_PERCENT : 100u));
    /* Let the first tracker frame be generated immediately on the first draw. */
    {
        uint64_t first_threshold =
            (uint64_t)g_music_fallback_frame_time * MUSIC_DISPLAY_RATE;
        g_music_fallback_phase = (first_threshold > 1000000ULL)
            ? (first_threshold - 1000000ULL)
            : 0;
    }
    g_music_initialized = 1;
}

static void update_background_music(void)
{
    struct xmp_frame_info fi;
    int ret;

    if (!g_music_initialized || !g_xmp_ctx) {
        return;
    }

    /* Finish queuing the previous tracker frame before asking libxmp for the
     * next one. This preserves samples when the AICA ringbuffer is temporarily
     * short on free space. One sample here means one interleaved L/R pair. */
    if (g_music_pending_buffer &&
        g_music_pending_offset < g_music_pending_samples) {
        unsigned int remaining =
            g_music_pending_samples - g_music_pending_offset;
        const void *src = g_music_pending_buffer +
            ((size_t)g_music_pending_offset * 4u);
        int written = audio_write_stereo_data((void *)src, remaining);

        if (written < 0) {
            return;
        }

        if ((unsigned int)written > remaining) {
            written = (int)remaining;
        }
        g_music_pending_offset += (unsigned int)written;

        if (g_music_pending_offset < g_music_pending_samples) {
            return;
        }

        g_music_pending_buffer = NULL;
        g_music_pending_samples = 0;
        g_music_pending_offset = 0;
    }

    /*
     * rom_browser_draw() is called once per vblank (nominally 60 Hz). Polling
     * a 20 ms timer from that loop quantizes tracker ticks to either one or
     * two display frames and can repeatedly turn 20 ms into 33 ms. A
     * fractional frame accumulator instead schedules the correct average
     * cadence smoothly: at 50 Hz it skips one out of every six display frames.
     */
    g_music_fallback_phase += 1000000ULL;
    {
        uint64_t threshold =
            (uint64_t)g_music_fallback_frame_time * MUSIC_DISPLAY_RATE;

        if (g_music_fallback_phase < threshold) {
            return;
        }
        g_music_fallback_phase -= threshold;
    }

    ret = xmp_play_frame(g_xmp_ctx);
    if (ret < 0) {
        xmp_restart_module(g_xmp_ctx);
        return;
    }

    xmp_get_frame_info(g_xmp_ctx, &fi);

    if (fi.buffer && fi.buffer_size >= 4) {
        /* libxmp reports buffer_size in bytes; 16-bit stereo = 4 bytes/pair. */
        unsigned int bytes = (unsigned int)fi.buffer_size;
        bytes &= ~3u;

        g_music_pending_buffer = (const uint8_t *)fi.buffer;
        g_music_pending_samples = bytes / 4u;
        g_music_pending_offset = 0;

        /* Mix the OGG effect into a persistent copy, never into libxmp's
         * internal buffer. The copy stays valid across partial AICA writes. */
        if (menu_select_sfx_has_active_voice() &&
            bytes <= sizeof(g_music_mix_buffer)) {
            memcpy(g_music_mix_buffer, fi.buffer, bytes);
            mix_menu_select_sfx(g_music_mix_buffer,
                                g_music_pending_samples);
            g_music_pending_buffer = (const uint8_t *)g_music_mix_buffer;
        }

        /* Try to queue this frame immediately. If the ringbuffer is full, the
         * remainder stays pending and is written on a later browser frame. */
        {
            int written = audio_write_stereo_data(
                (void *)g_music_pending_buffer,
                g_music_pending_samples
            );

            if (written < 0) {
                return;
            }
            if ((unsigned int)written > g_music_pending_samples) {
                written = (int)g_music_pending_samples;
            }
            g_music_pending_offset = (unsigned int)written;

            if (g_music_pending_offset >= g_music_pending_samples) {
                g_music_pending_buffer = NULL;
                g_music_pending_samples = 0;
                g_music_pending_offset = 0;
            }
        }
    }

    /*
     * libxmp's frame_time reports the module's scanned/base duration and does
     * not include the relative tempo factor. Apply the same inverse factor to
     * the 60 Hz scheduler so audio samples are produced at exactly 44.1 kHz.
     */
    {
        uint32_t frame_time = (fi.frame_time > 0)
            ? (uint32_t)fi.frame_time
            : 20000u;

        if (g_music_speed_applied) {
            frame_time = (uint32_t)(
                ((uint64_t)frame_time * 100ULL +
                 (MUSIC_SPEED_PERCENT / 2u)) / MUSIC_SPEED_PERCENT);
        }

        if (frame_time == 0) {
            frame_time = 1;
        }
        g_music_fallback_frame_time = frame_time;
    }
}

/* ------------------------------------------------------------------------- */
/* Initialization                                                            */
/* ------------------------------------------------------------------------- */

void rom_browser_init(void)
{
    int i;

    /* The browser can be entered again after a game/video-mode transition.
     * Drop our old descriptors first, then rebuild them against the current
     * TA texture allocator. If video_init() has already reset that allocator,
     * ta_texture_free() simply won't find the old VRAM addresses; the descriptor
     * allocations themselves are still released here. */
    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        release_cover_slot(i);
    }

    release_static_texture(&bg_tex);
    release_static_texture(&logo_tex);
    release_static_texture(&joy1_tex);
    release_static_texture(&joy2_tex);
    release_static_texture(&joy3_tex);
    release_static_texture(&btn_main_tex);

    if (!g_romfs_initialized) {
        romfs_init_default();
        g_romfs_initialized = 1;
    }
    scan_romfs();
    init_background_music();

    current_scroll = 0.0f;
    frame_counter = 0;
    g_scroll_initialized = 0;
    g_last_requested_selected = -1;
    g_priority_cover_index = -1;
    g_upload_active = 0;
    g_upload_slot = -1;
    g_upload_rom_index = -1;
    g_upload_pixels = NULL;
    g_upload_next_y = 0;
    g_upload_source_width = 0;
    g_upload_source_height = 0;

    /* The background asset must be the new 512x512 version. Its centered
     * 320x240 artwork area is enlarged 2x when drawn, preserving the same
     * 640x480 visible composition as the previous 1024x1024 asset. */
    if (background_png_data &&
        background_png_width == BACKGROUND_TEXTURE_SIZE &&
        background_png_height == BACKGROUND_TEXTURE_SIZE) {
        bg_tex = load_static_texture(
            background_png_data,
            background_png_width,
            background_png_height
        );
    }

    if (logo_png_data) {
        logo_tex = load_static_texture(
            logo_png_data,
            logo_png_width,
            logo_png_height
        );
    }

    if (joy1_png_data) {
        joy1_tex = load_static_texture(
            joy1_png_data,
            joy1_png_width,
            joy1_png_height
        );
    }

    if (joy2_png_data) {
        joy2_tex = load_static_texture(
            joy2_png_data,
            joy2_png_width,
            joy2_png_height
        );
    }

    if (joy3_png_data) {
        joy3_tex = load_static_texture(
            joy3_png_data,
            joy3_png_width,
            joy3_png_height
        );
    }

    if (btn_main_png_data) {
        btn_main_tex = load_static_texture(
            btn_main_png_data,
            btn_main_png_width,
            btn_main_png_height
        );
    }

    /*
     * Allocate the full-resolution pool once. Seven 512x512 ARGB4444 textures
     * use 3.5 MiB total. The extra slot gives cache replacement room while
     * preserving cards needed across the complete scroll transition.
     * If allocation stops early, keep every slot that succeeded rather than
     * repeatedly reallocating during browsing.
     */
    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        void *vram = ta_texture_malloc(COVER_TEXTURE_SIZE, 16);
        texture_description_t *desc;

        if (!vram) {
            break;
        }

        desc = ta_texture_desc_direct(
            vram,
            COVER_TEXTURE_SIZE,
            TA_TEXTUREMODE_ARGB4444
        );

        if (!desc) {
            ta_texture_free(vram);
            break;
        }

        g_cover_cache[i].vram = vram;
        g_cover_cache[i].desc = desc;
    }

    /* Covers are primed on the first draw, after the caller's selected index
     * is known. That avoids initially caching the wrong titles when the browser
     * is reopened with a non-zero selection. */

#ifdef FEATURE_FREETYPE
    {
        extern uint8_t *nes_font_ttf_data;
        extern unsigned int nes_font_ttf_len;

        if (!font_18pt) {
            font_18pt = font_add(nes_font_ttf_data, nes_font_ttf_len);
            if (font_18pt) {
                font_set_size(font_18pt, 16);
            }
        }

        if (!font_14pt) {
            font_14pt = font_add(nes_font_ttf_data, nes_font_ttf_len);
            if (font_14pt) {
                font_set_size(font_14pt, 12);
            }
        }

    }
#endif

}

int rom_browser_count(void)
{
    return g_rom_count;
}

const char *rom_browser_name(int index)
{
    if (index < 0 || index >= g_rom_count) {
        return NULL;
    }

    return g_rom_names[index];
}

/* ------------------------------------------------------------------------- */
/* Drawing helpers                                                           */
/* ------------------------------------------------------------------------- */

static void draw_footer(void)
{
    const int base_y = 446;
    const float btn_size = 40.0f;
    const float joy_size = 48.0f;
    const int text_y = 456;
    const int left_group_shift = 10;

    int joy_frame = (frame_counter / 20) % 4;
    texture_description_t *current_joy = joy1_tex;

    if (joy_frame == 1 && joy2_tex) {
        current_joy = joy2_tex;
    }
    else if (joy_frame == 2 && joy3_tex) {
        current_joy = joy3_tex;
    }
    else if (joy_frame == 3 && joy2_tex) {
        current_joy = joy2_tex;
    }

    if (current_joy && current_joy->height > 0) {
        float scale = joy_size / (float)current_joy->height;
        int width = (int)((float)current_joy->width * scale);
        int joy_y = base_y - (int)joy_size + 5;

        sprite_draw_scaled(20 + left_group_shift, joy_y, scale, scale, current_joy);

        if (font_14pt) {
            font_metrics_t metrics = font_get_text_metrics(font_14pt, "SELECT");
            int text_x = 20 + left_group_shift + ((width - (int)metrics.width) / 2);
            ta_draw_text(text_x, text_y,
                         font_14pt, rgb(255, 255, 255), "SELECT");
        }
    }

    if (btn_main_tex && btn_main_tex->height > 0) {
        float scale = btn_size / (float)btn_main_tex->height;
        int width = (int)((float)btn_main_tex->width * scale);
        int btn_y = base_y - (int)btn_size + 18;

        sprite_draw_scaled(85 + left_group_shift, btn_y, scale, scale, btn_main_tex);
        if (font_14pt) {
            font_metrics_t metrics = font_get_text_metrics(font_14pt, "A");
            int text_x = 85 + left_group_shift + ((width - (int)metrics.width) / 2);
            ta_draw_text(text_x, text_y,
                         font_14pt, rgb(255, 255, 255), "A");
        }

        sprite_draw_scaled(135 + left_group_shift, btn_y, scale, scale, btn_main_tex);
        if (font_14pt) {
            font_metrics_t metrics = font_get_text_metrics(font_14pt, "B");
            int text_x = 135 + left_group_shift + ((width - (int)metrics.width) / 2);
            ta_draw_text(text_x, text_y,
                         font_14pt, rgb(255, 255, 255), "B");
        }

        int start_x = (640 - width) / 2;
        sprite_draw_scaled(start_x, btn_y, scale, scale, btn_main_tex);
        if (font_14pt) {
            font_metrics_t metrics = font_get_text_metrics(font_14pt, "START");
            int text_x = start_x + ((width - (int)metrics.width) / 2);
            ta_draw_text(text_x, text_y,
                         font_14pt, rgb(255, 255, 255), "START");
        }
    }

#ifdef FEATURE_FREETYPE
    /*
     * NES-style life bar.
     * The dark track and red fill are drawn first; the four white frame
     * edges are drawn last as solid strips so neither horizontal edge can
     * be covered by the interior. Red is deliberately flat and hard-edged:
     * each segment is one solid pixel-art block, with no soft highlights,
     * shadows, or sub-block shading.
     */
    if (font_14pt && g_rom_count > 0) {
        enum {
            LIFE_BAR_WIDTH = 131,
            LIFE_BAR_HEIGHT = 20,
            LIFE_BAR_FRAME = 3,
            LIFE_BAR_SEGMENTS = 14,
            LIFE_BAR_SEGMENT_WIDTH = 8,
            LIFE_BAR_SEGMENT_GAP = 1,
            LIFE_BAR_TEXT_GAP = 14,
            LIFE_BAR_RIGHT_EDGE = 620
        };

        /* Seven deliberately stepped red tones, each used for two blocks. */
        static const uint8_t red_gradient[LIFE_BAR_SEGMENTS][3] = {
            {255,  64,  64}, {255,  64,  64},
            {255,  48,  48}, {255,  48,  48},
            {240,  32,  32}, {240,  32,  32},
            {224,  24,  24}, {224,  24,  24},
            {208,  16,  16}, {208,  16,  16},
            {184,  12,  12}, {184,  12,  12},
            {152,   8,   8}, {152,   8,   8}
        };

        char position_text[16];
        font_metrics_t metrics;
        int current_index;
        int text_x;
        int bar_x;
        int bar_y;
        int inner_x;
        int inner_y;
        int inner_width;
        int inner_height;
        int filled_pixels;
        int i;
        float progress;

        /* Show the nearest card while keeping the last card at N/N before wrap. */
        current_index = (int)floorf(current_scroll + 0.5f);
        if (current_index < 0) {
            current_index = 0;
        }
        if (current_index >= g_rom_count) {
            current_index = g_rom_count - 1;
        }

        snprintf(position_text, sizeof(position_text),
                 "%d : %d", current_index + 1, g_rom_count);

        metrics = font_get_text_metrics(font_14pt, "%s", position_text);
        text_x = LIFE_BAR_RIGHT_EDGE - (int)metrics.width;
        bar_x = text_x - LIFE_BAR_TEXT_GAP - LIFE_BAR_WIDTH;
        bar_y = text_y + ((int)metrics.height - LIFE_BAR_HEIGHT) / 2 - 3;

        inner_x = bar_x + LIFE_BAR_FRAME;
        inner_y = bar_y + LIFE_BAR_FRAME;
        inner_width = LIFE_BAR_WIDTH - (LIFE_BAR_FRAME * 2);
        inner_height = LIFE_BAR_HEIGHT - (LIFE_BAR_FRAME * 2);

        progress = (current_scroll + 1.0f) / (float)g_rom_count;
        if (progress < 0.0f) {
            progress = 0.0f;
        }
        if (progress > 1.0f) {
            progress = 1.0f;
        }

        filled_pixels = (int)floorf(
            (progress * (float)inner_width) + 0.5f
        );
        if (filled_pixels > inner_width) {
            filled_pixels = inner_width;
        }

        /* Recessed dark track. */
        sprite_draw_box(
            inner_x,
            inner_y,
            inner_x + inner_width - 1,
            inner_y + inner_height - 1,
            rgb(28, 28, 32)
        );

        /* Fourteen equal-height, solid-color blocks with crisp dark gaps. */
        for (i = 0; i < LIFE_BAR_SEGMENTS; i++) {
            int segment_start = i *
                (LIFE_BAR_SEGMENT_WIDTH + LIFE_BAR_SEGMENT_GAP);
            int segment_fill = filled_pixels - segment_start;

            if (segment_fill <= 0) {
                continue;
            }
            if (segment_fill > LIFE_BAR_SEGMENT_WIDTH) {
                segment_fill = LIFE_BAR_SEGMENT_WIDTH;
            }

            sprite_draw_box(
                inner_x + segment_start,
                inner_y,
                inner_x + segment_start + segment_fill - 1,
                inner_y + inner_height - 1,
                rgb(red_gradient[i][0],
                    red_gradient[i][1],
                    red_gradient[i][2])
            );
        }

        /*
         * Draw the side rails at full height. The shorter top and bottom
         * rails stop exactly at the inner edges of the side rails, creating
         * a clean pixel-panel joint without extending past the corners.
         */
        sprite_draw_box(
            bar_x,
            bar_y,
            bar_x + LIFE_BAR_FRAME - 1,
            bar_y + LIFE_BAR_HEIGHT - 1,
            rgb(255, 255, 255)
        );
        sprite_draw_box(
            bar_x + LIFE_BAR_WIDTH - LIFE_BAR_FRAME,
            bar_y,
            bar_x + LIFE_BAR_WIDTH - 1,
            bar_y + LIFE_BAR_HEIGHT - 1,
            rgb(255, 255, 255)
        );
        sprite_draw_box(
            bar_x + LIFE_BAR_FRAME,
            bar_y,
            bar_x + LIFE_BAR_WIDTH - LIFE_BAR_FRAME - 1,
            bar_y + LIFE_BAR_FRAME - 1,
            rgb(255, 255, 255)
        );
        sprite_draw_box(
            bar_x + LIFE_BAR_FRAME,
            bar_y + LIFE_BAR_HEIGHT - LIFE_BAR_FRAME,
            bar_x + LIFE_BAR_WIDTH - LIFE_BAR_FRAME - 1,
            bar_y + LIFE_BAR_HEIGHT - 1,
            rgb(255, 255, 255)
        );

        /* Position / library total. */
        ta_draw_text(
            text_x,
            text_y,
            font_14pt,
            rgb(255, 255, 255),
            "%s",
            position_text
        );
    }
#endif
}

/* ------------------------------------------------------------------------- */
/* Main browser frame                                                        */
/* ------------------------------------------------------------------------- */

void rom_browser_draw(int selected)
{
    int base_idx;
    float offset;

    update_background_music();

    if (g_rom_count > 0) {
        if (selected < 0) {
            selected = 0;
        }
        if (selected >= g_rom_count) {
            selected = g_rom_count - 1;
        }

        /* On the first frame, start at the caller's current selection. On
         * later frames retain the original eased carousel movement. */
        if (!g_scroll_initialized) {
            static const int preload_order[COVER_CACHE_SIZE] = {
                0, -1, 1, -2, 2, -3, 3
            };

            current_scroll = (float)selected;
            g_scroll_initialized = 1;
            g_last_requested_selected = selected;

            /* Fill the first viewport before its first display list is built.
             * The extra two entries prime both directions of the next move. */
            for (int i = 0; i < COVER_CACHE_SIZE; i++) {
                int index = wrap_index(selected + preload_order[i]);
                if (index >= 0 && !is_cover_loaded(index)) {
                    (void)load_cover_direct(index, selected);
                }
            }
        }
        else if (g_rom_count > 1) {
            float diff = shortest_scroll_diff(selected);

            /* Schedule the newly entering cover as soon as the requested
             * selection changes. Do not wait for the viewport boundary. */
            if (selected != g_last_requested_selected) {
                /* Do not play on the initial selection; only on user navigation. */
                if (g_last_requested_selected >= 0) {
                    play_menu_select_sfx();
                }

                if (fabsf(diff) >= 0.01f) {
                    int direction = (diff > 0.0f) ? 1 : -1;
                    g_priority_cover_index = wrap_index(selected + (direction * 2));
                }
                else {
                    g_priority_cover_index = -1;
                }
                g_last_requested_selected = selected;
            }

            if (fabsf(diff) < 0.01f) {
                current_scroll = (float)selected;
            }
            else {
                current_scroll += diff * 0.08f;
            }

            while (current_scroll >= (float)g_rom_count) {
                current_scroll -= (float)g_rom_count;
            }
            while (current_scroll < 0.0f) {
                current_scroll += (float)g_rom_count;
            }
        }
        else {
            current_scroll = 0.0f;
        }
    }
    else {
        selected = 0;
        current_scroll = 0.0f;
        g_last_requested_selected = -1;
        g_priority_cover_index = -1;
    }

    frame_counter++;

    /* Upload cover pixels in small bands. VRAM allocations and descriptors
     * remain fixed; no full 512x512 upload is performed in one navigation frame. */
    prepare_cover_cache(selected);

    ta_commit_begin();

    if (bg_tex) {
        sprite_draw_scaled(
            BACKGROUND_DRAW_X,
            BACKGROUND_DRAW_Y,
            BACKGROUND_DRAW_SCALE,
            BACKGROUND_DRAW_SCALE,
            bg_tex
        );
    }

    if (g_rom_count > 0) {
        const int center_x = 320;
        const int center_y = 210;

        base_idx = (int)floorf(current_scroll);
        offset = current_scroll - (float)base_idx;

        for (int i = -3; i <= 3; i++) {
            int index = wrap_index(base_idx + i);
            cover_cache_t *cover;
            texture_description_t *tex;
            float virtual_i;
            float scale;
            float texture_scale;
            int draw_y;
            int draw_x;
            int draw_width;
            int draw_height;

            if (index < 0) {
                continue;
            }

            cover = get_cover_slot_for_index(index);
            if (!cover || !cover->desc) {
                continue;
            }
            tex = cover->desc;

            virtual_i = (float)i - offset;
            if (fabsf(virtual_i) > 2.5f) {
                continue;
            }

            scale = 0.75f - (0.20f * fabsf(virtual_i));
            draw_y = center_y + (int)(fabsf(virtual_i) * 40.0f);
            draw_x = center_x + (int)(virtual_i * 280.0f);

            if (fabsf(virtual_i) < 0.1f) {
                int bob = (int)(sinf((float)frame_counter * 0.05f) * 8.0f);
                draw_y += bob;
            }

            /* The cache contains a complete 512x512 cover texture. */
            texture_scale = scale *
                ((float)cover->source_width / (float)COVER_TEXTURE_SIZE);
            draw_width = (int)((float)cover->source_width * scale);
            draw_height = (int)((float)cover->source_height * scale);

            sprite_draw_scaled(
                draw_x - (draw_width / 2),
                draw_y - (draw_height / 2),
                texture_scale,
                texture_scale,
                tex
            );
        }
    }

    if (logo_tex) {
        const float logo_scale = 0.30f;
        sprite_draw_scaled(10, -36, logo_scale, logo_scale, logo_tex);
    }

    if (g_rom_count == 0) {
#ifdef FEATURE_FREETYPE
        if (font_18pt) {
            const char *message = g_romfs_scan_failed
                ? "ROMFS ERROR"
                : "NO ROMS FOUND";
            ta_draw_text(260, 75, font_18pt,
                         rgb(255, 100, 100), "%s", message);
        }
#endif
    }
    else {
#ifdef FEATURE_FREETYPE
        if (font_18pt) {
            char display_name[ROM_NAME_LEN];
            const char *name = g_rom_names[selected];
            char *dot;
            int text_x;
            font_metrics_t metrics;

            strncpy(display_name, name, ROM_NAME_LEN - 1);
            display_name[ROM_NAME_LEN - 1] = '\0';

            dot = strrchr(display_name, '.');
            if (dot) {
                *dot = '\0';
            }

            for (int i = 0; display_name[i] != '\0'; i++) {
                display_name[i] = (char)toupper((unsigned char)display_name[i]);
            }

            metrics = font_get_text_metrics(font_18pt, "%s", display_name);
            text_x = (640 - (int)metrics.width) / 2;
            ta_draw_text(text_x, 400, font_18pt,
                         rgb(255, 255, 255), "%s", display_name);
        }
#endif
    }


    draw_footer();

    ta_commit_end();
    ta_render();
}
