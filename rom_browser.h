#ifndef ROM_BROWSER_H
#define ROM_BROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Scan ROMFS and cache .nes names */
void rom_browser_init(void);

int rom_browser_count(void);
const char *rom_browser_name(int index);

/* Draw ROM selection screen */
void rom_browser_draw(int selected);

#ifdef __cplusplus
}
#endif

#endif /* ROM_BROWSER_H */