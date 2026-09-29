// The result history: a variable-length ring in one block. Host build, plain malloc for the block.
#include "../main/ui/habitat/history.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_alloc;
static size_t last_request;
static void *alloc(size_t n)
{
    last_request = n;
    return fail_alloc ? NULL : calloc(1, n);
}
static void release(ht_history_t *h)
{
    free(h->buf);
    h->buf = NULL;
}
static void fresh(ht_history_t *h, size_t bytes)
{
    release(h);
    assert(ht_history_init(h, bytes, alloc));
}
static const char *text_of(const ht_history_t *h, const char *id, unsigned n)
{
    ht_history_entry_t e;
    assert(ht_history_get(h, id, n, &e));
    return e.text;
}

static void basics(void)
{
    ht_history_t h = {0};
    fresh(&h, 4096);
    assert(ht_history_enabled(&h) && ht_history_count(&h, "a") == 0 && last_request == 4096);
    assert(ht_history_add(&h, "a", "recap one", "first result", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "recap two", "second result", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "b", "other", "b's result", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "recap three", "third result", false) == HT_HISTORY_ADDED);
    // Newest first, per agent.
    assert(ht_history_count(&h, "a") == 3 && ht_history_count(&h, "b") == 1 && ht_history_count(&h, "c") == 0);
    assert(!strcmp(text_of(&h, "a", 0), "third result") && !strcmp(text_of(&h, "a", 1), "second result") &&
           !strcmp(text_of(&h, "a", 2), "first result") && !strcmp(text_of(&h, "b", 0), "b's result"));
    ht_history_entry_t e;
    assert(!ht_history_get(&h, "a", 3, &e) && !ht_history_get(&h, "c", 0, &e));
    assert(ht_history_get(&h, "a", 1, &e) && !strcmp(e.recap, "recap two") && e.text_len == strlen("second result"));
    // A sequence number finds its entry from any position, and reports how far back it is.
    uint32_t oldest, newest;
    assert(ht_history_get(&h, "a", 2, &e)); oldest = e.seq;
    assert(ht_history_get(&h, "a", 0, &e)); newest = e.seq;
    unsigned n = 99;
    assert(ht_history_find(&h, "a", oldest, &n) && n == 2);
    assert(ht_history_find(&h, "a", newest, &n) && n == 0);
    assert(!ht_history_find(&h, "b", oldest, &n) && !ht_history_find(&h, "a", 12345, &n));
    // Recap only, or text only: whichever there is. Nothing at all is refused.
    assert(ht_history_add(&h, "d", "just a recap", "", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "d", "", "just text", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "d", "", "", false) == HT_HISTORY_REJECTED);
    assert(ht_history_add(&h, "", "r", "t", false) == HT_HISTORY_REJECTED);
    assert(ht_history_add(&h, NULL, "r", "t", false) == HT_HISTORY_REJECTED);
    release(&h);
}

static void dedupe(void)
{
    ht_history_t h = {0};
    fresh(&h, 4096);
    assert(ht_history_add(&h, "a", "r1", "one", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "r2", "two", false) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "r3", "three", false) == HT_HISTORY_ADDED);
    // The daemon replays its last few summaries on every connect: none is new, whatever the order.
    assert(ht_history_add(&h, "a", "r1", "one", true) == HT_HISTORY_DUPLICATE);
    assert(ht_history_add(&h, "a", "r3", "three", true) == HT_HISTORY_DUPLICATE);
    assert(ht_history_add(&h, "a", "r2", "two", true) == HT_HISTORY_DUPLICATE);
    assert(ht_history_count(&h, "a") == 3);
    // A live result repeating the newest is one delivery twice; repeating an older one is a new result.
    assert(ht_history_add(&h, "a", "r3", "three", false) == HT_HISTORY_DUPLICATE);
    assert(ht_history_add(&h, "a", "r1", "one", false) == HT_HISTORY_ADDED);
    assert(ht_history_count(&h, "a") == 4 && !strcmp(text_of(&h, "a", 0), "one"));
    // Same text on another agent is its own entry; same length with different bytes is not a duplicate.
    assert(ht_history_add(&h, "b", "r1", "one", true) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "r1", "ONE", true) == HT_HISTORY_ADDED);
    assert(ht_history_add(&h, "a", "different recap", "three", true) == HT_HISTORY_ADDED);
    assert(ht_history_count(&h, "a") == 6 && ht_history_count(&h, "b") == 1);
    release(&h);
}

static void eviction(void)
{
    ht_history_t h = {0};
    fresh(&h, 1024);
    char text[64];
    unsigned added = 0;
    for (int i = 0; i < 100; i++) {
        snprintf(text, sizeof text, "result number %03d with some padding text", i);
        assert(ht_history_add(&h, "a", "recap", text, false) == HT_HISTORY_ADDED);
        added++;
        assert(ht_history_used(&h) <= 1024);
        // The newest is always there, and what survives is a contiguous newest-first run.
        assert(!strcmp(text_of(&h, "a", 0), text));
        unsigned count = ht_history_count(&h, "a");
        assert(count >= 1 && count <= added);
        for (unsigned k = 0; k < count; k++) {
            char want[64];
            snprintf(want, sizeof want, "result number %03d with some padding text", i - (int)k);
            assert(!strcmp(text_of(&h, "a", k), want));
        }
    }
    // The ring wrapped many times and holds far fewer than were added.
    assert(ht_history_count(&h, "a") < 30 && ht_history_count(&h, "a") > 5);
    // An old id's entries leave with the ring, not before.
    release(&h);
    fresh(&h, 1024);
    assert(ht_history_add(&h, "old", "r", "long ago", false) == HT_HISTORY_ADDED);
    for (int i = 0; i < 40; i++) {
        snprintf(text, sizeof text, "fill %03d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
        assert(ht_history_add(&h, "new", "r", text, false) == HT_HISTORY_ADDED);
    }
    assert(ht_history_count(&h, "old") == 0 && ht_history_count(&h, "new") > 3);
    release(&h);
}

// Randomised sizes against a model: everything the ring still holds must be exactly what was added,
// in order, for whichever suffix survived. Exercises the wrap filler, the short-tail case and every
// eviction path.
static void fuzz(void)
{
    ht_history_t h = {0};
    static char *added[20000];
    static char *ids[20000];
    unsigned long state = 12345;
#define RND(n) ((state = state * 6364136223846793005ul + 1442695040888963407ul), (unsigned)((state >> 33) % (n)))
    for (int round = 0; round < 6; round++) {
        size_t bytes = round == 0 ? 512 : round == 1 ? 1500 : round == 2 ? 9000 : round == 3 ? 20000 : round == 4 ? 65536 : 3000;
        fresh(&h, bytes);
        int total = 0;
        for (int i = 0; i < 3000; i++) {
            unsigned len = RND(4) == 0 ? RND(3000) : RND(200);
            if (round == 0) len %= 200;
            char *t = malloc(len + 1);
            for (unsigned k = 0; k < len; k++) t[k] = (char)('a' + RND(26));
            t[len] = 0;
            char id[8];
            snprintf(id, sizeof id, "id%u", RND(3));
            ht_history_result_t r = ht_history_add(&h, id, "recap", t, false);
            if (r == HT_HISTORY_ADDED && len <= HT_HISTORY_TEXT_MAX) {
                added[total] = t; ids[total] = strdup(id); total++;
            } else free(t);
            assert(ht_history_used(&h) <= h.cap && h.count <= (unsigned)total);
            // Verify the tail of the model for each agent against the ring.
            if (i % 37 == 0 || i > 2990) {
                for (int who = 0; who < 3; who++) {
                    char want_id[8];
                    snprintf(want_id, sizeof want_id, "id%d", who);
                    unsigned count = ht_history_count(&h, want_id), k = 0;
                    for (int m = total - 1; m >= 0 && k < count; m--) {
                        if (strcmp(ids[m], want_id)) continue;
                        ht_history_entry_t e;
                        assert(ht_history_get(&h, want_id, k, &e));
                        size_t want = strlen(added[m]);
                        // A text the whole ring cannot hold is cut, never anything smaller.
                        if (want > e.text_len) assert(bytes < want + 300);
                        assert(!strncmp(e.text, added[m], e.text_len) && e.text[e.text_len] == 0);
                        k++;
                    }
                    assert(k == count);
                }
            }
        }
        for (int i = 0; i < total; i++) { free(added[i]); free(ids[i]); }
        release(&h);
    }
}

static void truncation(void)
{
    ht_history_t h = {0};
    // Fits: never cut, right up to the per-entry limit and including multibyte text.
    fresh(&h, 512 * 1024);
    static char big[9000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    big[8100] = 0;
    assert(ht_history_add(&h, "a", "r", big, false) == HT_HISTORY_ADDED);
    ht_history_entry_t e;
    assert(ht_history_get(&h, "a", 0, &e) && e.text_len == 8100 && strlen(e.text) == 8100);
    // The frame carries up to about 8 KB; the limit itself is exact.
    memset(big, 'y', sizeof big - 1);
    big[HT_HISTORY_TEXT_MAX] = 0;
    assert(ht_history_add(&h, "a", "r", big, false) == HT_HISTORY_ADDED);
    assert(ht_history_get(&h, "a", 0, &e) && e.text_len == HT_HISTORY_TEXT_MAX);
    // Over the per-entry limit: cut, on a character boundary. "é" is two bytes, so the cut cannot land
    // in the middle of one however the limit falls.
    for (int offset = 0; offset < 4; offset++) {
        char *p = big;
        for (int i = 0; i < offset; i++) *p++ = 'x';
        while (p < big + 8500) { *p++ = (char)0xc3; *p++ = (char)0xa9; }
        *p = 0;
        assert(ht_history_add(&h, "u", "r", big, false) == HT_HISTORY_ADDED);
        assert(ht_history_get(&h, "u", 0, &e));
        assert(e.text_len <= HT_HISTORY_TEXT_MAX && e.text_len >= HT_HISTORY_TEXT_MAX - 1);
        for (size_t i = offset; i < e.text_len; i += 2) assert((unsigned char)e.text[i] == 0xc3 && (unsigned char)e.text[i + 1] == 0xa9);
    }
    // Three-byte and four-byte sequences too.
    const char *seqs[] = {"\xe2\x82\xac", "\xf0\x9f\x99\x82"};
    for (int k = 0; k < 2; k++) {
        size_t w = strlen(seqs[k]);
        char *p = big;
        while (p + w < big + 8600) { memcpy(p, seqs[k], w); p += w; }
        *p = 0;
        assert(ht_history_add(&h, "s", "r", big, false) == HT_HISTORY_ADDED);
        assert(ht_history_get(&h, "s", 0, &e) && e.text_len % w == 0 && e.text_len <= HT_HISTORY_TEXT_MAX);
    }
    release(&h);

    // Only when it cannot fit: a ring too small for the text keeps the largest whole-character prefix
    // and nothing else is cut; the neighbour entries remain intact.
    fresh(&h, 600);
    assert(ht_history_add(&h, "a", "r", "short and whole", false) == HT_HISTORY_ADDED);
    memset(big, 'z', 2000);
    big[2000] = 0;
    assert(ht_history_add(&h, "b", "r", big, false) == HT_HISTORY_ADDED);
    assert(ht_history_get(&h, "b", 0, &e) && e.text_len > 400 && e.text_len < 600);
    assert(ht_history_count(&h, "a") == 0);   // it had to make room for a whole ring's worth
    assert(ht_history_add(&h, "a", "r", "short and whole", false) == HT_HISTORY_ADDED);
    assert(!strcmp(text_of(&h, "a", 0), "short and whole"));
    // The same with multibyte text against a small ring: still no split character.
    for (int i = 0; i < 400; i++) { big[i * 2] = (char)0xc3; big[i * 2 + 1] = (char)0xa9; }
    big[800] = 0;
    assert(ht_history_add(&h, "m", "r", big, false) == HT_HISTORY_ADDED);
    assert(ht_history_get(&h, "m", 0, &e) && e.text_len % 2 == 0 && e.text_len < 600);
    for (size_t i = 0; i < e.text_len; i += 2) assert((unsigned char)e.text[i] == 0xc3);
    // A recap longer than its cap is cut on a boundary as well.
    static char recap[400];
    for (int i = 0; i < 199; i++) { recap[i * 2] = (char)0xc3; recap[i * 2 + 1] = (char)0xa9; }
    recap[398] = 0;
    fresh(&h, 4096);
    assert(ht_history_add(&h, "r", recap, "body", false) == HT_HISTORY_ADDED);
    assert(ht_history_get(&h, "r", 0, &e) && strlen(e.recap) == HT_HISTORY_RECAP_MAX && !strcmp(e.text, "body"));
    // A ring smaller than one header cannot be built.
    release(&h);
    assert(!ht_history_init(&h, 32, alloc) && !ht_history_enabled(&h));
}

static void allocation_failure(void)
{
    ht_history_t h;
    memset(&h, 0xff, sizeof h);
    fail_alloc = true;
    assert(!ht_history_init(&h, 512 * 1024, alloc));
    fail_alloc = false;
    // Disabled means inert, not crashing: every call answers as an empty history.
    assert(!ht_history_enabled(&h) && last_request == 512 * 1024);
    assert(ht_history_add(&h, "a", "r", "text", false) == HT_HISTORY_REJECTED);
    ht_history_entry_t e;
    unsigned n;
    assert(ht_history_count(&h, "a") == 0 && !ht_history_get(&h, "a", 0, &e) && !ht_history_find(&h, "a", 1, &n));
    assert(ht_history_used(&h) == 0);
    assert(!ht_history_init(&h, 512 * 1024, NULL));
}

int main(void)
{
    basics();
    dedupe();
    eviction();
    fuzz();
    truncation();
    allocation_failure();
    puts("history: newest first, per-agent filtering, replay dedupe, wraparound eviction (fuzzed), UTF-8-safe truncation only when it cannot fit, allocation failure PASS");
    return 0;
}
