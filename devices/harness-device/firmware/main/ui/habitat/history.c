#include "history.h"
#include <string.h>

typedef struct {
    uint32_t seq, hash, length;   // length and hash are of the text as it arrived, before any cut
    uint16_t total, text_len, recap_len;
    uint8_t id_len, filler;
} record_t;
enum { HDR = sizeof(record_t) };

static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }
static record_t rec(const ht_history_t *h, uint32_t pos)
{
    record_t r;
    memcpy(&r, h->buf + pos, sizeof r);
    return r;
}
// A tail too short for a header, or one marked as filler, means the next record is at the start.
static bool wraps(const ht_history_t *h, uint32_t pos)
{
    return h->cap - pos < HDR || rec(h, pos).filler;
}
static const char *id_of(const ht_history_t *h, uint32_t pos) { return (const char *)h->buf + pos + HDR; }
static uint32_t hash_of(const char *text, const char *recap)
{
    uint32_t x = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) x = (x ^ *p) * 16777619u;
    x = (x ^ 0xff) * 16777619u;
    for (const unsigned char *p = (const unsigned char *)recap; *p; p++) x = (x ^ *p) * 16777619u;
    return x;
}
// The largest prefix of s no longer than limit that does not end inside a UTF-8 sequence.
static size_t whole_chars(const char *s, size_t length, size_t limit)
{
    if (length <= limit) return length;
    while (limit && ((unsigned char)s[limit] & 0xc0) == 0x80) limit--;
    return limit;
}
bool ht_history_init(ht_history_t *h, size_t bytes, ht_history_alloc_t alloc)
{
    memset(h, 0, sizeof *h);
    if (!alloc || bytes < 4 * HDR || bytes > 0x7fffffffu) return false;
    h->buf = alloc(bytes);
    if (!h->buf) return false;
    h->cap = (uint32_t)bytes & ~3u;
    return true;
}
bool ht_history_enabled(const ht_history_t *h) { return h->buf != NULL; }
uint32_t ht_history_used(const ht_history_t *h) { return h->used; }

// Visit records oldest first. `fn` returns true to stop.
typedef bool (*visit_fn)(const ht_history_t *, uint32_t pos, const record_t *, void *);
static void walk(const ht_history_t *h, visit_fn fn, void *ctx)
{
    uint32_t pos = h->head, seen = 0;
    while (seen < h->count) {
        if (wraps(h, pos)) pos = 0;
        record_t r = rec(h, pos);
        if (fn(h, pos, &r, ctx)) return;
        pos += r.total;
        seen++;
    }
}
static void evict_oldest(ht_history_t *h)
{
    if (wraps(h, h->head)) { h->used -= h->cap - h->head; h->head = 0; }
    record_t r = rec(h, h->head);
    h->head += r.total;
    h->used -= r.total;
    h->count--;
    if (!h->count) h->head = h->tail = h->used = 0;
}
typedef struct {
    const char *id;
    uint32_t length, hash;
    bool anywhere, found;
} dup_t;
static bool dup_visit(const ht_history_t *h, uint32_t pos, const record_t *r, void *v)
{
    dup_t *d = v;
    if (strcmp(id_of(h, pos), d->id)) return false;
    // Only the newest matters for a live result: keep overwriting until the walk ends.
    d->found = r->length == d->length && r->hash == d->hash;
    return d->found && d->anywhere;
}
ht_history_result_t ht_history_add(ht_history_t *h, const char *id, const char *recap,
                                   const char *text, bool replay)
{
    if (!h->buf || !id || !*id || strlen(id) > 200) return HT_HISTORY_REJECTED;
    if (!recap) recap = "";
    if (!text) text = "";
    if (!*text && !*recap) return HT_HISTORY_REJECTED;
    size_t id_len = strlen(id), text_len = strlen(text);
    dup_t d = {.id = id, .length = (uint32_t)text_len, .hash = hash_of(text, recap), .anywhere = replay};
    walk(h, dup_visit, &d);
    if (d.found) return HT_HISTORY_DUPLICATE;

    size_t recap_len = whole_chars(recap, strlen(recap), HT_HISTORY_RECAP_MAX);
    size_t fixed = HDR + id_len + 1 + recap_len + 1 + 1 + 3;   // NULs, and the round-up to four
    if (fixed >= h->cap) return HT_HISTORY_REJECTED;
    size_t room = h->cap - fixed;
    if (room > HT_HISTORY_TEXT_MAX) room = HT_HISTORY_TEXT_MAX;
    size_t kept = whole_chars(text, text_len, room);
    uint32_t total = align4((uint32_t)(HDR + id_len + 1 + recap_len + 1 + kept + 1));

    for (;;) {
        bool wrap = h->cap - h->tail < total;
        uint32_t gap = wrap ? h->cap - h->tail : 0;
        if (h->used + gap + total <= h->cap) {
            if (wrap && gap >= HDR) {
                record_t f = {.filler = 1};
                memcpy(h->buf + h->tail, &f, sizeof f);
            }
            h->used += gap;
            if (wrap) h->tail = 0;
            break;
        }
        evict_oldest(h);
    }
    record_t r = {.seq = ++h->seq, .hash = d.hash, .length = d.length, .total = (uint16_t)total,
                  .text_len = (uint16_t)kept, .recap_len = (uint16_t)recap_len, .id_len = (uint8_t)id_len};
    uint8_t *at = h->buf + h->tail;
    memset(at, 0, total);
    memcpy(at, &r, sizeof r);
    memcpy(at + HDR, id, id_len);
    memcpy(at + HDR + id_len + 1, recap, recap_len);
    memcpy(at + HDR + id_len + 1 + recap_len + 1, text, kept);
    h->tail += total;
    h->used += total;
    h->count++;
    return HT_HISTORY_ADDED;
}

typedef struct {
    const char *id;
    unsigned matches, want, index;   // want counts from the oldest
    uint32_t seq;
    bool by_seq, found;
    ht_history_entry_t *out;
} pick_t;
static void fill(const ht_history_t *h, uint32_t pos, const record_t *r, ht_history_entry_t *out)
{
    const char *recap = id_of(h, pos) + r->id_len + 1;
    out->seq = r->seq;
    out->recap = recap;
    out->text = recap + r->recap_len + 1;
    out->text_len = r->text_len;
}
static bool count_visit(const ht_history_t *h, uint32_t pos, const record_t *r, void *v)
{
    pick_t *p = v;
    (void)r;
    if (!strcmp(id_of(h, pos), p->id)) p->matches++;
    return false;
}
static bool pick_visit(const ht_history_t *h, uint32_t pos, const record_t *r, void *v)
{
    pick_t *p = v;
    if (strcmp(id_of(h, pos), p->id)) return false;
    if (p->by_seq ? r->seq == p->seq : p->matches == p->want) {
        p->found = true;
        p->index = p->matches;
        if (p->out) fill(h, pos, r, p->out);
        return true;
    }
    p->matches++;
    return false;
}
unsigned ht_history_count(const ht_history_t *h, const char *id)
{
    pick_t p = {.id = id};
    if (!h->buf || !id) return 0;
    walk(h, count_visit, &p);
    return p.matches;
}
bool ht_history_get(const ht_history_t *h, const char *id, unsigned n, ht_history_entry_t *out)
{
    unsigned total = ht_history_count(h, id);
    if (n >= total) return false;
    pick_t p = {.id = id, .want = total - 1 - n, .out = out};
    walk(h, pick_visit, &p);
    return p.found;
}
bool ht_history_find(const ht_history_t *h, const char *id, uint32_t seq, unsigned *n)
{
    unsigned total = ht_history_count(h, id);
    pick_t p = {.id = id, .seq = seq, .by_seq = true};
    if (!total) return false;
    walk(h, pick_visit, &p);
    if (p.found && n) *n = total - 1 - p.index;
    return p.found;
}
