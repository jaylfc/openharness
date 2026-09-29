#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A SMALL HISTORY OF EACH AGENT'S RESULTS, for the reader.
 *
 * The pane cache keeps one result per agent, cut to PANE_RESULT_BYTES, and it lives in a fixed table
 * that cannot hold more without growing every slot. This is the other shape: one variable-length ring
 * in a single block the caller allocates (PSRAM on the board, so the internal heap never sees it),
 * holding whatever a summary frame carried, oldest evicted first. The struct below is all that lives in
 * internal RAM.
 *
 * Records are contiguous, and a record that would straddle the end of the block is moved to the start
 * instead, so an entry's text is one NUL-terminated string the renderer can read in place.
 */
enum { HT_HISTORY_TEXT_MAX = 8192, HT_HISTORY_RECAP_MAX = 240 };

typedef struct {
    uint8_t *buf;
    uint32_t cap, head, tail, used, count, seq;
} ht_history_t;

typedef struct {
    uint32_t seq;
    const char *recap, *text;   // valid until the next add, and only under the caller's lock
    uint32_t text_len;
} ht_history_entry_t;

typedef enum { HT_HISTORY_ADDED, HT_HISTORY_DUPLICATE, HT_HISTORY_REJECTED } ht_history_result_t;
typedef void *(*ht_history_alloc_t)(size_t);

// Takes `bytes` from `alloc`. False leaves the history disabled: every call below then does nothing.
bool ht_history_init(ht_history_t *h, size_t bytes, ht_history_alloc_t alloc);
bool ht_history_enabled(const ht_history_t *h);
/*
 * `replay` is a summary the daemon is filling back in (attach, refresh). Those are dropped when this
 * agent already holds the same text anywhere; a live one only when it repeats the agent's newest.
 * Text longer than HT_HISTORY_TEXT_MAX, or than the whole ring can hold, is cut at a character
 * boundary; nothing else is ever cut.
 */
ht_history_result_t ht_history_add(ht_history_t *h, const char *id, const char *recap,
                                   const char *text, bool replay);
// One agent's entries. n = 0 is the newest.
unsigned ht_history_count(const ht_history_t *h, const char *id);
bool ht_history_get(const ht_history_t *h, const char *id, unsigned n, ht_history_entry_t *out);
// Position (0 = newest) of the entry with this sequence number, or false once it has been evicted.
bool ht_history_find(const ht_history_t *h, const char *id, uint32_t seq, unsigned *n);
uint32_t ht_history_used(const ht_history_t *h);
