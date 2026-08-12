#include "rom_browser.h"

#include <naomi/video.h>
#include <naomi/romfs.h>

#include <dirent.h>
#include <string.h>

#define MAX_ROMS 32

static char rom_names[MAX_ROMS][64];
static int rom_count = 0;

static void scan_romfs_nes_files(void)
{
    DIR *dir = opendir("rom://");
    struct dirent *entry;

    rom_count = 0;

    while (dir && (entry = readdir(dir)) != NULL && rom_count < MAX_ROMS)
    {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".nes") == 0)
        {
            strncpy(rom_names[rom_count], entry->d_name, 63);
            rom_names[rom_count][63] = 0;
            rom_count++;
        }
    }

    if (dir) closedir(dir);
}

void rom_browser_init(void)
{
    /* Keep init here so main doesn't need to know about romfs/dirent */
    romfs_init_default();
    scan_romfs_nes_files();
}

int rom_browser_count(void)
{
    return rom_count;
}

const char *rom_browser_name(int index)
{
    if (index < 0 || index >= rom_count) return NULL;
    return rom_names[index];
}

void rom_browser_draw(int selected)
{
    video_draw_debug_text(60, 40, rgb(200,255,120), "CPU+PPU HARNESS - ROM SEC (NROM)");

    for (int i = 0; i < rom_count; i++) {
        int y = 90 + i * 28;
        video_draw_debug_text(70, y,
                              (i == selected) ? rgb(255,255,0) : rgb(200,200,200),
                              (i == selected) ? "> %s" : "  %s",
                              rom_names[i]);
    }

    video_draw_debug_text(60, 230, rgb(120,200,255),
                          "Yukari/Asagi: Sec | button1/RETURN: Yukle");
}