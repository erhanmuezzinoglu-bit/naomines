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
#include <limits.h>

#define MAX_ROMS          128
#define ROM_NAME_LEN      64
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
static font_t *font_24pt = NULL;

static texture_description_t *bg_tex = NULL;
static texture_description_t *logo_tex = NULL;
static texture_description_t *joy1_tex = NULL;
static texture_description_t *joy2_tex = NULL;
static texture_description_t *joy3_tex = NULL;
static texture_description_t *btn_main_tex = NULL;

typedef struct {
    void *vram;
    texture_description_t *desc;
    int rom_index;
    int last_used;
    unsigned int source_width;
    unsigned int source_height;
} cover_cache_t;

static cover_cache_t g_cover_cache[COVER_CACHE_SIZE];

/* Shared staging buffer: one cover is converted/uploaded at a time. */
static uint16_t g_cover_upload_pixels[COVER_TEXTURE_PIXELS]
    __attribute__((aligned(32)));

static float current_scroll = 0.0f;
static int frame_counter = 0;
static int g_romfs_initialized = 0;
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

    if (dir) {
        closedir(dir);
    }
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
    int oldest = INT_MAX;
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

        if (!g_cover_cache[i].vram || !g_cover_cache[i].desc) {
            continue;
        }
        if (cached_index == protected_index ||
            is_cover_needed_during_transition(cached_index, selected)) {
            continue;
        }

        if (g_cover_cache[i].last_used < oldest) {
            oldest = g_cover_cache[i].last_used;
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
        g_upload_active = 0;
        g_upload_slot = -1;
        g_upload_rom_index = -1;
        g_upload_pixels = NULL;
        g_upload_next_y = 0;
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

    for (i = 0; i < COVER_CACHE_SIZE; i++) {
        g_cover_cache[i].vram = NULL;
        g_cover_cache[i].desc = NULL;
        g_cover_cache[i].rom_index = -1;
        g_cover_cache[i].last_used = 0;
        g_cover_cache[i].source_width = 0;
        g_cover_cache[i].source_height = 0;
    }

    /* The background asset must be the new 512x512 version. Its centered
     * 320x240 artwork area is enlarged 2x when drawn, preserving the same
     * 640x480 visible composition as the previous 1024x1024 asset. */
    if (background_png_data &&
        background_png_width == BACKGROUND_TEXTURE_SIZE &&
        background_png_height == BACKGROUND_TEXTURE_SIZE) {
        bg_tex = ta_texture_desc_malloc_direct(
            BACKGROUND_TEXTURE_SIZE,
            background_png_data,
            TA_TEXTUREMODE_ARGB4444
        );
    }

    if (logo_png_data) {
        logo_tex = ta_texture_desc_malloc_direct(
            (int)logo_png_width,
            logo_png_data,
            TA_TEXTUREMODE_ARGB4444
        );
    }

    if (joy1_png_data) {
        joy1_tex = ta_texture_desc_malloc_direct(
            (int)joy1_png_width,
            joy1_png_data,
            TA_TEXTUREMODE_ARGB4444
        );
    }

    if (joy2_png_data) {
        joy2_tex = ta_texture_desc_malloc_direct(
            (int)joy2_png_width,
            joy2_png_data,
            TA_TEXTUREMODE_ARGB4444
        );
    }

    if (joy3_png_data) {
        joy3_tex = ta_texture_desc_malloc_direct(
            (int)joy3_png_width,
            joy3_png_data,
            TA_TEXTUREMODE_ARGB4444
        );
    }

    if (btn_main_png_data) {
        btn_main_tex = ta_texture_desc_malloc_direct(
            (int)btn_main_png_width,
            btn_main_png_data,
            TA_TEXTUREMODE_ARGB4444
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
        extern uint8_t *dejavusans_ttf_data;
        extern unsigned int dejavusans_ttf_len;

        if (!font_18pt) {
            font_18pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
            if (font_18pt) {
                font_set_size(font_18pt, 18);
            }
        }

        if (!font_14pt) {
            font_14pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
            if (font_14pt) {
                font_set_size(font_14pt, 14);
            }
        }

        if (!font_24pt) {
            font_24pt = font_add(dejavusans_ttf_data, dejavusans_ttf_len);
            if (font_24pt) {
                font_set_size(font_24pt, 24);
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
    const int base_y = 450;
    const float btn_size = 40.0f;
    const float joy_size = 48.0f;
    const int text_y = 455;

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
        int joy_y = base_y - (int)joy_size + 10;

        sprite_draw_scaled(20, joy_y, scale, scale, current_joy);

        if (font_18pt) {
            ta_draw_text(20 + (width / 2) - 25, text_y,
                         font_18pt, rgb(255, 255, 255), "Select");
        }
    }

    if (btn_main_tex && btn_main_tex->height > 0) {
        float scale = btn_size / (float)btn_main_tex->height;
        int width = (int)((float)btn_main_tex->width * scale);
        int btn_y = base_y - (int)btn_size + 18;

        sprite_draw_scaled(85, btn_y, scale, scale, btn_main_tex);
        if (font_18pt) {
            ta_draw_text(85 + (width / 2) - 5, text_y,
                         font_18pt, rgb(255, 255, 255), "A");
        }

        sprite_draw_scaled(135, btn_y, scale, scale, btn_main_tex);
        if (font_18pt) {
            ta_draw_text(135 + (width / 2) - 5, text_y,
                         font_18pt, rgb(255, 255, 255), "B");
        }

        sprite_draw_scaled(195, btn_y, scale, scale, btn_main_tex);
        if (font_18pt) {
            ta_draw_text(195 + (width / 2) - 18, text_y,
                         font_18pt, rgb(255, 255, 255), "Start");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Main browser frame                                                        */
/* ------------------------------------------------------------------------- */

void rom_browser_draw(int selected)
{
    int base_idx;
    float offset;

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
        const float logo_scale = 0.25f;
        sprite_draw_scaled(10, -25, logo_scale, logo_scale, logo_tex);
    }

    if (g_rom_count == 0) {
#ifdef FEATURE_FREETYPE
        if (font_18pt) {
            ta_draw_text(260, 75, font_18pt,
                         rgb(255, 100, 100), "NO ROMS FOUND");
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

            strncpy(display_name, name, ROM_NAME_LEN - 1);
            display_name[ROM_NAME_LEN - 1] = '\0';

            dot = strrchr(display_name, '.');
            if (dot) {
                *dot = '\0';
            }

            text_x = 320 - (int)((float)strlen(display_name) * 4.5f);
            ta_draw_text(text_x, 400, font_18pt,
                         rgb(255, 255, 255), "%s", display_name);
        }
#endif
    }

    draw_footer();

    ta_commit_end();
    ta_render();
}
