#pragma once
// The menu a double shake opens. Pure data and predicates: no ESP-IDF, no scene. ui_habitat.c draws the
// rows and runs the actions; this file says WHAT the rows are and WHEN each one may be used, so the table
// is where a custom function is added later and the rules are host-tested (test/test_shake_menu.c).
//
// Fork only. To add a row: add an action id below, one line to the table in shake_menu.c, and one case
// where ui_habitat.c runs ht_menu_action_t (menu_run). Nothing else needs to know.
#include <stdbool.h>

typedef enum {
    HT_MENU_KEYBOARD,   // type a message to the agent on screen
    HT_MENU_READ,       // the reader: that agent's results, or "Nothing to read yet."
    HT_MENU_SETTINGS,   // the controls list
    HT_MENU_QUIET,      // toggle: the character stops animating
    HT_MENU_MUTE,       // toggle: the finished-turn beep
    HT_MENU_LAUNCHER,   // back to the taOS Orb launcher (launcher_return.h)
} ht_menu_action_t;

// What the enable rules may look at. Filled by the caller under the display lock.
typedef struct {
    bool connected;   // a daemon session is up
    bool agent;       // there is an agent on screen
    bool quiet, muted;
    bool launcher;    // flashed as a guest of the taOS Orb launcher (a factory app exists)
} ht_menu_ctx_t;

typedef struct {
    const char *label;                            // at most HT_MENU_LABEL_CELLS with a toggle's state
    ht_menu_action_t action;
    bool (*enabled)(const ht_menu_ctx_t *);       // NULL = always
    int (*state)(const ht_menu_ctx_t *);          // NULL = not a toggle; else 0 off, 1 on
} ht_menu_item_t;

#define HT_MENU_LABEL_CELLS 18   // one row: 306 px of the 17 px cell

int ht_menu_count(void);
const ht_menu_item_t *ht_menu_item(int i);        // NULL out of range
bool ht_menu_enabled(int i, const ht_menu_ctx_t *ctx);
// "Quiet mode: off" for a toggle, the label as is otherwise.
void ht_menu_label(int i, const ht_menu_ctx_t *ctx, char out[24]);

// May a shake open the menu right now? Only from the two main faces, with the screen on, no recording or
// upload in progress and no finger down. The caller passes what it knows; nothing here reads a view.
bool ht_menu_may_open(bool on_main_face, bool asleep, bool voice_busy, bool touching);

// Row layout. Up to HT_MENU_ROWS_ALL rows fit on the glass at once; a longer table shows
// HT_MENU_ROWS_PAGED per page under the pager the models list already uses.
#define HT_MENU_ROWS_ALL 6
#define HT_MENU_ROWS_PAGED 3
#define HT_MENU_TOP 108
#define HT_MENU_PITCH 44
#define HT_MENU_X 59
#define HT_MENU_W 348
#define HT_MENU_TEXT_X 80
#define HT_MENU_TEXT_W 306
int ht_menu_rows(int count);                      // rows drawn per page for a table of `count`
