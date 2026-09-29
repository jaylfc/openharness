#include "keyboard.h"
#include <string.h>

// Rows: how many keys, and where the row starts. Row 3 is widths, not a count of equal keys.
static const uint8_t row_keys[HT_KB_ROWS] = {10, 9, 7, 5};
static const int16_t row_x[HT_KB_ROWS] = {13, 35, 79, 68};
static const int16_t bottom_w[5] = {60, 60, 96, 60, 54};
static const uint8_t bottom_kind[5] = {HT_KEY_SHIFT, HT_KEY_SYMBOLS, HT_KEY_SPACE, HT_KEY_DELETE, HT_KEY_SEND};
#define KEY_W 44

static const char letters[] = "qwertyuiopasdfghjklzxcvbnm";
static const char symbols[] = "1234567890-/:;()&@\".,?!'_+";   // 10 + 9 + 7

void ht_kb_reset(ht_keyboard_t *kb) { memset(kb, 0, sizeof *kb); }

ht_key_t ht_kb_key(int i)
{
    ht_key_t k = {0, 0, KEY_W, HT_KB_KEY_H, HT_KEY_CHAR};
    if (i < 0 || i >= HT_KB_KEYS) { k.w = k.h = 0; return k; }
    int row = 0, first = 0;
    while (i >= first + row_keys[row]) first += row_keys[row++];
    int n = i - first;
    k.y = (int16_t)(HT_KB_TOP + row * HT_KB_KEY_H);
    if (row < 3) {
        k.x = (int16_t)(row_x[row] + n * KEY_W);
    } else {
        int x = row_x[3];
        for (int j = 0; j < n; j++) x += bottom_w[j];
        k.x = (int16_t)x;
        k.w = bottom_w[n];
        k.kind = (ht_key_kind_t)bottom_kind[n];
    }
    return k;
}

int ht_kb_key_at(int x, int y)
{
    for (int i = 0; i < HT_KB_KEYS; i++) {
        ht_key_t k = ht_kb_key(i);
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return i;
    }
    return -1;
}

// Character keys are the first 26 indices (10 + 9 + 7); the bottom row follows.
static char key_char(const ht_keyboard_t *kb, int i)
{
    if (kb->page == HT_KB_SYMBOLS) return symbols[i];
    char c = letters[i];
    return kb->shift ? (char)(c - 'a' + 'A') : c;
}

void ht_kb_label(const ht_keyboard_t *kb, int i, char out[8])
{
    out[0] = 0;
    ht_key_t k = ht_kb_key(i);
    if (!k.w) return;
    switch (k.kind) {
    case HT_KEY_CHAR: out[0] = key_char(kb, i); out[1] = 0; break;
    case HT_KEY_SHIFT: strcpy(out, kb->shift ? "SHIFT" : "shift"); break;
    case HT_KEY_SYMBOLS: strcpy(out, kb->page == HT_KB_LETTERS ? "123" : "abc"); break;
    case HT_KEY_SPACE: strcpy(out, "space"); break;
    case HT_KEY_DELETE: strcpy(out, "del"); break;
    case HT_KEY_SEND: strcpy(out, "send"); break;
    }
}

bool ht_kb_can_send(const ht_keyboard_t *kb)
{
    for (size_t i = 0; i < kb->len; i++)
        if (kb->text[i] != ' ' && (kb->text[i] < '\t' || kb->text[i] > '\r')) return true;
    return false;
}

bool ht_kb_key_enabled(const ht_keyboard_t *kb, int i)
{
    ht_key_t k = ht_kb_key(i);
    if (!k.w) return false;
    switch (k.kind) {
    case HT_KEY_CHAR:
    case HT_KEY_SPACE: return kb->len < HT_KB_MAX;
    case HT_KEY_SHIFT: return kb->page == HT_KB_LETTERS;
    case HT_KEY_SYMBOLS: return true;
    case HT_KEY_DELETE: return kb->len > 0;
    case HT_KEY_SEND: return ht_kb_can_send(kb);
    }
    return false;
}

static void push(ht_keyboard_t *kb, char c)
{
    kb->text[kb->len++] = c;
    kb->text[kb->len] = 0;
}

ht_kb_result_t ht_kb_press(ht_keyboard_t *kb, int i)
{
    if (!ht_kb_key_enabled(kb, i)) return HT_KB_NOTHING;
    switch (ht_kb_key(i).kind) {
    case HT_KEY_CHAR:
        push(kb, key_char(kb, i));
        kb->shift = false;
        return HT_KB_CHANGED;
    case HT_KEY_SPACE:
        push(kb, ' ');
        return HT_KB_CHANGED;
    case HT_KEY_SHIFT:
        kb->shift = !kb->shift;
        return HT_KB_CHANGED;
    case HT_KEY_SYMBOLS:
        kb->page = kb->page == HT_KB_LETTERS ? HT_KB_SYMBOLS : HT_KB_LETTERS;
        kb->shift = false;
        return HT_KB_CHANGED;
    case HT_KEY_DELETE:
        // One character, not one byte: step back over UTF-8 continuation bytes.
        do kb->len--; while (kb->len > 0 && ((unsigned char)kb->text[kb->len] & 0xC0) == 0x80);
        kb->text[kb->len] = 0;
        return HT_KB_CHANGED;
    case HT_KEY_SEND:
        return HT_KB_SEND;
    }
    return HT_KB_NOTHING;
}

size_t ht_kb_take(ht_keyboard_t *kb, char *out, size_t cap)
{
    if (!out || !cap || !ht_kb_can_send(kb)) return 0;
    size_t a = 0, b = kb->len;
    while (a < b && (kb->text[a] == ' ' || (kb->text[a] >= '\t' && kb->text[a] <= '\r'))) a++;
    while (b > a && (kb->text[b - 1] == ' ' || (kb->text[b - 1] >= '\t' && kb->text[b - 1] <= '\r'))) b--;
    if (b - a >= cap) return 0;
    memcpy(out, kb->text + a, b - a);
    out[b - a] = 0;
    size_t n = b - a;
    ht_kb_reset(kb);
    return n;
}

void ht_kb_display(const ht_keyboard_t *kb, char *out, size_t cap)
{
    if (!cap) return;
    size_t n = kb->len;
    if (n + 2 > cap) {
        n = cap - 2;
        while (n > 0 && ((unsigned char)kb->text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, kb->text, n);
    out[n] = '_';
    out[n + 1] = 0;
}
