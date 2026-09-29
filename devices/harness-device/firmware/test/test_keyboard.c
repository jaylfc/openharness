// The on-screen keyboard: geometry against the round glass, hit mapping, and the typing state machine.
#include "../main/ui/habitat/keyboard.h"
#include "../main/ui/habitat/terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int by_label(const ht_keyboard_t *kb, const char *want)
{
    char l[8];
    for (int i = 0; i < HT_KB_KEYS; i++) { ht_kb_label(kb, i, l); if (!strcmp(l, want)) return i; }
    return -1;
}
static void type(ht_keyboard_t *kb, const char *s)
{
    char one[2] = {0, 0};
    for (; *s; s++) {
        int i;
        if (*s == ' ') i = by_label(kb, "space");
        else { one[0] = *s; i = by_label(kb, one); }
        assert(i >= 0);
        assert(ht_kb_press(kb, i) == HT_KB_CHANGED);
    }
}
static bool inside(int x, int y, int r)
{
    int dx = x - HT_KB_FACE / 2, dy = y - HT_KB_FACE / 2;
    return dx * dx + dy * dy <= r * r;
}

int main(void)
{
    ht_keyboard_t kb;
    ht_kb_reset(&kb);

    // GEOMETRY: every corner of every key inside the circle less the bezel margin, targets big enough,
    // and no two keys overlap. Arithmetic, not a picture.
    for (int i = 0; i < HT_KB_KEYS; i++) {
        ht_key_t k = ht_kb_key(i);
        assert(k.w >= 44 && k.h >= 44);
        assert(inside(k.x, k.y, HT_KB_SAFE_RADIUS) && inside(k.x + k.w, k.y, HT_KB_SAFE_RADIUS));
        assert(inside(k.x, k.y + k.h, HT_KB_SAFE_RADIUS) && inside(k.x + k.w, k.y + k.h, HT_KB_SAFE_RADIUS));
        assert(HT_KB_SAFE_RADIUS <= 233 - 8);
        for (int j = i + 1; j < HT_KB_KEYS; j++) {
            ht_key_t o = ht_kb_key(j);
            assert(k.x + k.w <= o.x || o.x + o.w <= k.x || k.y + k.h <= o.y || o.y + o.h <= k.y);
        }
        // hit mapping: the centre and all four inside corners land on this key and nothing else
        assert(ht_kb_key_at(k.x + k.w / 2, k.y + k.h / 2) == i);
        assert(ht_kb_key_at(k.x, k.y) == i && ht_kb_key_at(k.x + k.w - 1, k.y + k.h - 1) == i);
    }
    assert(ht_kb_key(HT_KB_KEYS).w == 0 && ht_kb_key(-1).w == 0);
    assert(ht_kb_key_at(2, 200) == -1 && ht_kb_key_at(233, 100) == -1 && ht_kb_key_at(233, 400) == -1);
    // rows are contiguous: no gap a thumb could fall into between two keys of the same row
    for (int i = 0; i + 1 < HT_KB_KEYS; i++) {
        ht_key_t a = ht_kb_key(i), b = ht_kb_key(i + 1);
        if (a.y == b.y) assert(a.x + a.w == b.x);
    }
    // the typed text sits above the keys, wholly on the glass, in a column that fits the runs budget
    assert(HT_KB_TEXT_Y + HT_KB_TEXT_ROWS * HT_KB_TEXT_LINE <= HT_KB_TOP);
    for (int r = 0; r <= HT_KB_TEXT_ROWS; r++) {
        int y = HT_KB_TEXT_Y + r * HT_KB_TEXT_LINE;
        assert(inside(HT_KB_TEXT_X, y, HT_KB_SAFE_RADIUS) && inside(HT_KB_TEXT_X + HT_KB_TEXT_W, y, HT_KB_SAFE_RADIUS));
    }
    assert(HT_KB_KEYS + HT_KB_TEXT_ROWS + 1 /* back */ <= HT_RUNS);

    // LABELS: 26 distinct letters, capitals under shift, symbols on their page
    char l[8], seen[27] = {0};
    for (int i = 0; i < 26; i++) { ht_kb_label(&kb, i, l); assert(l[0] >= 'a' && l[0] <= 'z' && !l[1]); seen[l[0] - 'a']++; }
    for (int c = 0; c < 26; c++) assert(seen[c] == 1);
    ht_kb_label(&kb, 0, l); assert(!strcmp(l, "q"));
    assert(ht_kb_press(&kb, by_label(&kb, "shift")) == HT_KB_CHANGED && kb.shift);
    ht_kb_label(&kb, 0, l); assert(!strcmp(l, "Q"));

    // SHIFT is one-shot: the next letter is a capital and shift is off again; twice is off
    ht_kb_reset(&kb);
    assert(ht_kb_press(&kb, by_label(&kb, "shift")) == HT_KB_CHANGED);
    type(&kb, "H"); assert(!kb.shift && !strcmp(kb.text, "H"));
    type(&kb, "i"); assert(!strcmp(kb.text, "Hi"));
    ht_kb_press(&kb, by_label(&kb, "shift")); ht_kb_press(&kb, by_label(&kb, "SHIFT")); assert(!kb.shift);

    // SYMBOLS: the page toggles, shift is off there and on return, digits and punctuation type
    ht_kb_press(&kb, by_label(&kb, "shift"));
    assert(ht_kb_press(&kb, by_label(&kb, "123")) == HT_KB_CHANGED);
    assert(kb.page == HT_KB_SYMBOLS && !kb.shift);
    assert(!ht_kb_key_enabled(&kb, by_label(&kb, "shift")) && ht_kb_press(&kb, by_label(&kb, "shift")) == HT_KB_NOTHING);
    type(&kb, "42?");
    assert(!strcmp(kb.text, "Hi42?"));
    assert(by_label(&kb, "q") < 0 && by_label(&kb, "abc") >= 0);
    assert(ht_kb_press(&kb, by_label(&kb, "abc")) == HT_KB_CHANGED && kb.page == HT_KB_LETTERS && !kb.shift);
    // every symbol key really types its own label
    ht_kb_press(&kb, by_label(&kb, "123"));
    for (int i = 0; i < 26; i++) { ht_kb_reset(&kb); kb.page = HT_KB_SYMBOLS; ht_kb_label(&kb, i, l); ht_kb_press(&kb, i); assert(kb.text[0] == l[0] && kb.len == 1); }

    // DELETE: nothing on empty; one character at a time; whole UTF-8 characters, never half of one
    ht_kb_reset(&kb);
    assert(!ht_kb_key_enabled(&kb, by_label(&kb, "del")) && ht_kb_press(&kb, by_label(&kb, "del")) == HT_KB_NOTHING && kb.len == 0);
    type(&kb, "ab"); ht_kb_press(&kb, by_label(&kb, "del")); assert(!strcmp(kb.text, "a"));
    strcpy(kb.text, "a\xC3\xA9"); kb.len = 3;                       // a, e-acute (2 bytes)
    ht_kb_press(&kb, by_label(&kb, "del")); assert(!strcmp(kb.text, "a") && kb.len == 1);
    strcpy(kb.text, "\xE2\x82\xAC\xF0\x9F\x99\x82"); kb.len = 7;    // euro (3), smiley (4)
    ht_kb_press(&kb, by_label(&kb, "del")); assert(kb.len == 3);
    ht_kb_press(&kb, by_label(&kb, "del")); assert(kb.len == 0 && !kb.text[0]);
    ht_kb_press(&kb, by_label(&kb, "del")); assert(kb.len == 0);

    // SEND: off when empty or only white space, on with a real character, returns the trimmed text
    ht_kb_reset(&kb);
    int send = by_label(&kb, "send");
    assert(!ht_kb_key_enabled(&kb, send) && ht_kb_press(&kb, send) == HT_KB_NOTHING);
    type(&kb, "   ");
    assert(!ht_kb_can_send(&kb) && ht_kb_press(&kb, send) == HT_KB_NOTHING);
    char out[HT_KB_MAX + 1];
    assert(ht_kb_take(&kb, out, sizeof out) == 0 && kb.len == 3);   // refuses, and leaves the buffer alone
    type(&kb, "hi there ");
    assert(ht_kb_key_enabled(&kb, send) && ht_kb_press(&kb, send) == HT_KB_SEND);
    assert(ht_kb_press(&kb, send) == HT_KB_SEND);                     // pressing does not consume; take does
    assert(ht_kb_take(&kb, out, 4) == 0 && kb.len == 12);             // a buffer too small takes nothing
    size_t n = ht_kb_take(&kb, out, sizeof out);
    assert(n == 8 && !strcmp(out, "hi there") && kb.len == 0 && !kb.text[0]);
    assert(!ht_kb_key_enabled(&kb, send));

    // LENGTH CAP: fills to HT_KB_MAX bytes, then characters and space are refused, delete still works
    ht_kb_reset(&kb);
    int a = by_label(&kb, "a");
    for (int i = 0; i < HT_KB_MAX; i++) assert(ht_kb_press(&kb, a) == HT_KB_CHANGED);
    assert(kb.len == HT_KB_MAX && strlen(kb.text) == HT_KB_MAX);
    assert(ht_kb_press(&kb, a) == HT_KB_NOTHING && ht_kb_press(&kb, by_label(&kb, "space")) == HT_KB_NOTHING);
    assert(kb.len == HT_KB_MAX && !ht_kb_key_enabled(&kb, a));
    assert(ht_kb_key_enabled(&kb, send) && ht_kb_key_enabled(&kb, by_label(&kb, "123")));
    ht_kb_press(&kb, by_label(&kb, "del")); assert(kb.len == HT_KB_MAX - 1 && ht_kb_key_enabled(&kb, a));
    assert(ht_kb_take(&kb, out, sizeof out) == HT_KB_MAX - 1);       // the whole cap fits the caller's buffer
    // the wire limit: even every byte escaped as \u00XX stays far inside one frame (8192)
    assert(HT_KB_MAX * 6 + 64 < 8192);

    // DISPLAY: a cursor, whole characters when it has to cut
    ht_kb_reset(&kb); type(&kb, "ok");
    char d[16]; ht_kb_display(&kb, d, sizeof d); assert(!strcmp(d, "ok_"));
    strcpy(kb.text, "ab\xC3\xA9"); kb.len = 4;
    ht_kb_display(&kb, d, 5); assert(!strcmp(d, "ab_"));            // room for 3 bytes + cursor would split the e-acute
    ht_kb_display(&kb, d, 6); assert(!strcmp(d, "ab\xC3\xA9_"));

    printf("keyboard: PASS (%d keys inside the circle, hits, labels, shift, symbols, delete, send, cap %d); state=%zu bytes\n",
           HT_KB_KEYS, HT_KB_MAX, sizeof kb);
    return 0;
}
