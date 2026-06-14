#pragma once
#include "raylib.h"
#include <stdbool.h>

/* Load every skin under ~/.config/gcc-notch/skins/<dir>/skin.xml.
 * Requires an active GL context (call after InitWindow). Restores the
 * previously-selected skin if one was saved. Returns the number loaded. */
int skin_load_all(void);
void skin_unload_all(void);

int skin_count(void);
const char *skin_name(int i);   /* display name (xml "name" or dir name) */
const char *skin_author(int i); /* xml "author", or "" */
void skin_reload(void);         /* unload + reload all skins from disk */
int skin_current(void);         /* active index, -1 if none */
void skin_select(int i);        /* select + persist the choice */
void skin_next(void);           /* cycle to the next skin */
bool skin_have(void);           /* a skin is loaded and active */
bool skin_size(int *w, int *h); /* active skin bg pixel size; false if none */

void skin_set_font(Font f);    /* font used for on-screen stick values */
void skin_set_values(bool on); /* show/hide the numeric stick readout */
void skin_set_remap_display(bool on); /* false: draw the source device raw */
/* Draw the active skin, scaled to fit and centered in win_w x win_h.
   Does NOT clear the background (caller controls chroma key). */
void skin_draw(int win_w, int win_h);
