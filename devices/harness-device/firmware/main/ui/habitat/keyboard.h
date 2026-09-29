#pragma once
// An on-screen QWERTY for the round 466 face. Pure: no ESP-IDF, no scene, no allocation. The renderer
// reads the layout and the labels from here and draws them; the touch path maps a hit to a key index and
// calls ht_kb_press. Host-tested in test/test_keyboard.c, including that every key corner is on the glass.
//
// Fork only. What the typed text is FOR (a message to the agent on screen) is the caller's business.
//
// THE LAYOUT. Four rows of 48 px keys, contiguous, 44 px wide or more, so a thumb has a target and no
// dead gap between two keys:
//     row 0  10 keys  y 192
//     row 1   9 keys  y 240
//     row 2   7 keys  y 288
//     row 3   5 keys  y 336   shift, symbols, space, delete, send
// Widths are the widest that keep every corner inside HT_KB_SAFE_RADIUS of the face centre, which is the
// circle less an 8 px margin for the bezel. The typed text sits above them, three rows of the 20 px mono.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HT_KB_MAX 480          // bytes typed; the wire carries 8192 per frame, this is 6% of it even escaped
#define HT_KB_KEYS 31
#define HT_KB_ROWS 4
#define HT_KB_KEY_H 48
#define HT_KB_TOP 192
#define HT_KB_FACE 466
#define HT_KB_SAFE_RADIUS 225
#define HT_KB_TEXT_X 59
#define HT_KB_TEXT_Y 108
#define HT_KB_TEXT_W 348
#define HT_KB_TEXT_ROWS 3
#define HT_KB_TEXT_LINE 28     // ht_mono_20 cell height

typedef enum { HT_KEY_CHAR, HT_KEY_SHIFT, HT_KEY_SYMBOLS, HT_KEY_SPACE, HT_KEY_DELETE, HT_KEY_SEND } ht_key_kind_t;
typedef enum { HT_KB_LETTERS, HT_KB_SYMBOLS } ht_kb_page_t;
typedef enum { HT_KB_NOTHING, HT_KB_CHANGED, HT_KB_SEND } ht_kb_result_t;

typedef struct {
    int16_t x, y, w, h;
    ht_key_kind_t kind;
} ht_key_t;

typedef struct {
    ht_kb_page_t page;
    bool shift;               // one-shot: the next letter is a capital, then it is off again
    uint16_t len;
    char text[HT_KB_MAX + 1];
} ht_keyboard_t;

void ht_kb_reset(ht_keyboard_t *kb);
// Geometry of key `i` (0..HT_KB_KEYS-1). The same on both pages.
ht_key_t ht_kb_key(int i);
// The key whose rectangle holds this point, or -1. Rectangles do not overlap.
int ht_kb_key_at(int x, int y);
// What key `i` shows on the current page: one character, or a short word. Always NUL-terminated.
void ht_kb_label(const ht_keyboard_t *kb, int i, char out[8]);
// False for keys that do nothing now: shift on the symbols page, send with nothing to send, delete when
// empty, and any character key once the text is full.
bool ht_kb_key_enabled(const ht_keyboard_t *kb, int i);
bool ht_kb_can_send(const ht_keyboard_t *kb);   // at least one character that is not white space
ht_kb_result_t ht_kb_press(ht_keyboard_t *kb, int i);
// The text as it will be sent (leading and trailing white space cut) into `out`, and the buffer cleared.
// Returns the byte length, 0 (and leaves the buffer alone) when there is nothing to send.
size_t ht_kb_take(ht_keyboard_t *kb, char *out, size_t cap);
// The text with a trailing cursor, for drawing. Truncates at whole characters. `cap` at least 2.
void ht_kb_display(const ht_keyboard_t *kb, char *out, size_t cap);
