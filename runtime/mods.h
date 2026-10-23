// Game mods (Android): folders in mods/ in the app's folder, each with a romfs/ tree laid out as emulators take them
// (Eden, Ryujinx: <mod>/romfs/<the game's own paths>). Their files replace the game's when the game starts
// (services/fs.c) - a fan translation, retextures... Every mod found is on unless turned off in the menu's Mods page
// (ui/mods_tab.mm; larecomp.ini mods_off). Files the game doesn't have are left out (replacements only).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define MODS_MAX 32
typedef struct {
    char name[96];  // its folder's name
    bool on;
} ModInfo;

// The mods in mods/ (sorted by name), up to max
int mods_list(ModInfo *out, int max);
// Turns one on or off (larecomp.ini: settings_save after)
void mods_set(const char *name, bool on);
// The game opens its RomFS: the files of the mods that are on, looked up by mods_romfs_override from then on
void mods_scan(void);
// Host path of the file replacing RomFS file `path` ("regionUS/USen/message/System.msbt"): the first mod that's on (by
// name) with that file. False: none.
bool mods_romfs_override(const char *path, char *out, size_t cap);
// The mod's own note - a .txt in its folder (Sobre.txt, README.txt...) - its first `cap` bytes, in the menu font's
// characters (ASCII and accented Latin letters; symbols and emoji left out). "" if it has none.
const char *mods_note(const char *name, char *buf, size_t cap);
