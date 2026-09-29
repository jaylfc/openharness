// The shake menu's table and its rules: what the rows are, when each may be used, when a shake may open it.
#include "../main/ui/habitat/shake_menu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    // the table, in order, every action once, every label fits a row with its state
    assert(ht_menu_count() == 5);
    static const ht_menu_action_t want[] = {HT_MENU_KEYBOARD, HT_MENU_HISTORY, HT_MENU_SETTINGS, HT_MENU_QUIET, HT_MENU_MUTE};
    ht_menu_ctx_t c = {.connected = true, .agent = true};
    for (int i = 0; i < ht_menu_count(); i++) {
        assert(ht_menu_item(i)->action == want[i]);
        char label[24];
        ht_menu_label(i, &c, label);
        assert(label[0] && strlen(label) <= HT_MENU_LABEL_CELLS);
        c.quiet = c.muted = true;
        ht_menu_label(i, &c, label);
        assert(strlen(label) <= HT_MENU_LABEL_CELLS);
        c.quiet = c.muted = false;
    }
    assert(!ht_menu_item(-1) && !ht_menu_item(5) && !ht_menu_enabled(5, &c) && !ht_menu_enabled(-1, &c));

    // toggle labels show their state; plain rows do not
    char l[24];
    ht_menu_label(3, &c, l); assert(!strcmp(l, "Quiet mode: off"));
    ht_menu_label(4, &c, l); assert(!strcmp(l, "Mute sounds: off"));
    c.quiet = c.muted = true;
    ht_menu_label(3, &c, l); assert(!strcmp(l, "Quiet mode: on"));
    ht_menu_label(4, &c, l); assert(!strcmp(l, "Mute sounds: on"));
    ht_menu_label(0, &c, l); assert(!strcmp(l, "Keyboard"));
    c.quiet = c.muted = false;

    // enable rules: a message needs an agent AND a session; the reader needs an agent; the rest always work
    ht_menu_ctx_t none = {0}, link_only = {.connected = true}, agent_only = {.agent = true};
    assert(ht_menu_enabled(0, &c) && !ht_menu_enabled(0, &none) && !ht_menu_enabled(0, &link_only) && !ht_menu_enabled(0, &agent_only));
    assert(ht_menu_enabled(1, &c) && !ht_menu_enabled(1, &none) && ht_menu_enabled(1, &agent_only));
    for (int i = 2; i < 5; i++) assert(ht_menu_enabled(i, &none) && ht_menu_enabled(i, &c));

    // when a shake may open it: only the main faces, awake, idle audio, no finger
    for (int m = 0; m < 2; m++) for (int a = 0; a < 2; a++) for (int v = 0; v < 2; v++) for (int t = 0; t < 2; t++)
        assert(ht_menu_may_open(m, a, v, t) == (m && !a && !v && !t));

    // layout: a table that fits shows every row; a longer one pages three at a time
    assert(ht_menu_rows(0) == 0 && ht_menu_rows(1) == 1 && ht_menu_rows(HT_MENU_ROWS_ALL) == HT_MENU_ROWS_ALL);
    assert(ht_menu_rows(HT_MENU_ROWS_ALL + 1) == HT_MENU_ROWS_PAGED && ht_menu_rows(40) == HT_MENU_ROWS_PAGED);
    // every row, in both layouts, stays inside the circle less an 8 px bezel margin, at least 44 px tall
    int last_bottom = HT_MENU_TOP + HT_MENU_ROWS_ALL * HT_MENU_PITCH;
    assert(HT_MENU_PITCH >= 44);
    for (int y = HT_MENU_TOP; y <= last_bottom; y += HT_MENU_PITCH)
        for (int x = HT_MENU_X; x <= HT_MENU_X + HT_MENU_W; x += HT_MENU_W) {
            int dx = x - 233, dy = y - 233;
            assert(dx * dx + dy * dy <= 225 * 225);
        }
    printf("shake menu table: PASS (%d rows, rules, labels, open gate, layout)\n", ht_menu_count());
    return 0;
}
