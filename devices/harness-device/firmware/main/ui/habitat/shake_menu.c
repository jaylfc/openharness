#include "shake_menu.h"
#include <stdio.h>

static bool needs_agent(const ht_menu_ctx_t *c) { return c->agent; }
static bool needs_agent_link(const ht_menu_ctx_t *c) { return c->agent && c->connected; }
static int quiet_state(const ht_menu_ctx_t *c) { return c->quiet ? 1 : 0; }
static int muted_state(const ht_menu_ctx_t *c) { return c->muted ? 1 : 0; }

// THE TABLE. Order is the order on screen.
static const ht_menu_item_t items[] = {
    {"Keyboard",    HT_MENU_KEYBOARD, needs_agent_link, NULL},   // a message needs an agent AND a daemon
    {"History",     HT_MENU_HISTORY,  needs_agent,      NULL},   // the reader works from what the dial holds
    {"Settings",    HT_MENU_SETTINGS, NULL,             NULL},
    {"Quiet mode",  HT_MENU_QUIET,    NULL,             quiet_state},
    {"Mute sounds", HT_MENU_MUTE,     NULL,             muted_state},
};

int ht_menu_count(void) { return (int)(sizeof items / sizeof items[0]); }
const ht_menu_item_t *ht_menu_item(int i) { return i >= 0 && i < ht_menu_count() ? &items[i] : NULL; }

bool ht_menu_enabled(int i, const ht_menu_ctx_t *ctx)
{
    const ht_menu_item_t *it = ht_menu_item(i);
    return it && (!it->enabled || it->enabled(ctx));
}

void ht_menu_label(int i, const ht_menu_ctx_t *ctx, char out[24])
{
    const ht_menu_item_t *it = ht_menu_item(i);
    if (!it) { out[0] = 0; return; }
    if (it->state) snprintf(out, 24, "%s: %s", it->label, it->state(ctx) ? "on" : "off");
    else snprintf(out, 24, "%s", it->label);
}

bool ht_menu_may_open(bool on_main_face, bool asleep, bool voice_busy, bool touching)
{
    return on_main_face && !asleep && !voice_busy && !touching;
}

int ht_menu_rows(int count) { return count <= HT_MENU_ROWS_ALL ? count : HT_MENU_ROWS_PAGED; }
