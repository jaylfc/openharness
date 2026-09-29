#include "shake_detect.h"
#include <string.h>

static int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

void ht_shake_reset(ht_shake_t *s) { memset(s, 0, sizeof *s); }

uint32_t ht_shake_magnitude_mg(int ax, int ay, int az)
{
    // Milli-g squared fits 64 bits with room to spare, and a bitwise square root needs no libm.
    uint64_t sq = (uint64_t)((int64_t)ax * ax) + (uint64_t)((int64_t)ay * ay) + (uint64_t)((int64_t)az * az);
    uint64_t root = 0, bit = 1ull << 62;
    while (bit > sq) bit >>= 2;
    while (bit) {
        if (sq >= root + bit) { sq -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)root;
}

bool ht_shake_feed(ht_shake_t *s, uint32_t now, int ax, int ay, int az, bool asleep)
{
    if (asleep) {
        ht_shake_reset(s);
        s->has_strong = true;   // the stillness clock starts when the screen comes back
        s->last_strong = now;
        return false;
    }
    if (s->has_strong && since(now, s->last_strong) > (int32_t)SHAKE_STALE_MS) s->has_strong = false;
    if (s->in_group && since(now, s->group_last) > (int32_t)SHAKE_STALE_MS) s->in_group = false;
    if (s->started && since(now, s->last_start) > (int32_t)SHAKE_STALE_MS) s->started = false;
    if (s->cooling && since(now, s->cool_until) >= 0) s->cooling = false;

    uint32_t mag = ht_shake_magnitude_mg(ax, ay, az);
    uint32_t dev = mag > 1000 ? mag - 1000 : 1000 - mag;
    bool strong = dev >= SHAKE_JOLT_MG;

    // Settle: a group of two or more ends when nothing has happened for SHAKE_SETTLE_MS after its last jolt.
    if (s->in_group && s->jolts >= 2 && !strong && since(now, s->group_last) >= (int32_t)SHAKE_SETTLE_MS) {
        bool fire = !s->cooling && since(s->group_last, s->group_first) <= (int32_t)SHAKE_GROUP_MAX_MS;
        s->in_group = false;
        s->jolts = 0;
        if (fire) {
            s->cooling = true;
            s->cool_until = now + SHAKE_COOLDOWN_MS;
            return true;
        }
    }
    // A lone knock expires without a trace.
    if (s->in_group && s->jolts < 2 && since(now, s->group_last) > (int32_t)SHAKE_PAIR_GAP_MS) {
        s->in_group = false;
        s->jolts = 0;
    }
    if (dev < SHAKE_RELEASE_MG) s->in_jolt = false;

    if (strong) {
        bool quiet = !s->has_strong || since(now, s->last_strong) >= (int32_t)SHAKE_QUIET_MS;
        bool edge = !s->in_jolt && (!s->started || since(now, s->last_start) >= (int32_t)SHAKE_MIN_GAP_MS);
        if (edge) {
            s->in_jolt = s->started = true;
            s->last_start = now;
            if (s->in_group) {
                if (since(now, s->group_last) > (int32_t)SHAKE_PAIR_GAP_MS) {
                    s->in_group = false;   // too slow to be one gesture; the pause needs stillness again
                    s->jolts = 0;
                } else if (since(now, s->group_first) > (int32_t)SHAKE_GROUP_MAX_MS) {
                    s->in_group = false;   // a train, not a gesture: wait for stillness
                    s->jolts = 0;
                } else {
                    if (s->jolts < 255) s->jolts++;
                    s->group_last = now;
                }
            } else if (quiet && !s->cooling) {
                s->in_group = true;
                s->jolts = 1;
                s->group_first = s->group_last = now;
            }
        }
        s->has_strong = true;
        s->last_strong = now;
    }
    return false;
}
