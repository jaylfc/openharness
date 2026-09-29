// Synthetic 100 Hz traces through the double-shake detector: what must fire, and every way it must not.
#include "../main/shake_detect.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define STEP 10   // 100 Hz
typedef struct { uint32_t t; int amp; } jolt_t;   // an impulse on z: `amp` mg above gravity for 30 ms

static int fires;
static uint32_t fired_at[16];

// Feed [from, to) ms. `t` is the SAMPLE clock the detector sees; `base` shifts it (wrap tests).
// Gravity is straight down z unless `tilt` says the vector turns 90 degrees over the span.
static void run(ht_shake_t *s, uint32_t base, uint32_t from, uint32_t to, const jolt_t *j, int n, bool tilt,
                uint32_t asleep_from, uint32_t asleep_to)
{
    for (uint32_t t = from; t < to; t += STEP) {
        double ang = tilt ? 1.5707963 * (double)(t - from) / (double)(to - from) : 0;
        int ax = (int)lround(1000 * sin(ang)), az = (int)lround(1000 * cos(ang));
        for (int i = 0; i < n; i++)
            if (t >= j[i].t && t < j[i].t + 30) az += j[i].amp;
        bool asleep = t >= asleep_from && t < asleep_to;
        if (ht_shake_feed(s, base + t, ax + (int)(t % 7) - 3, (int)(t % 5) - 2, az, asleep)) {
            if (fires < 16) fired_at[fires] = t;
            fires++;
        }
    }
}
static void fresh(ht_shake_t *s) { ht_shake_reset(s); fires = 0; }

int main(void)
{
    ht_shake_t s;
    const int BIG = 2800;

    // magnitude: axis-aligned, a 3-4-5 vector, and the +-4 g extremes without overflow
    assert(ht_shake_magnitude_mg(0, 0, 1000) == 1000);
    assert(ht_shake_magnitude_mg(600, 800, 0) == 1000);
    assert(ht_shake_magnitude_mg(-3, 4, 0) == 5);
    assert(ht_shake_magnitude_mg(0, 0, 0) == 0);
    assert(ht_shake_magnitude_mg(32767, 32767, 32767) == 56754);

    // a deliberate double shake fires once, about SHAKE_SETTLE_MS after its last jolt
    fresh(&s);
    jolt_t two[] = {{3000, BIG}, {3400, BIG}};
    run(&s, 0, 0, 10000, two, 2, false, 1, 0);
    assert(fires == 1);
    assert(fired_at[0] >= 3400 + SHAKE_SETTLE_MS && fired_at[0] <= 3400 + SHAKE_SETTLE_MS + 60);

    // the same shake with the pair a full 1.1 s apart, still inside SHAKE_PAIR_GAP_MS
    fresh(&s);
    jolt_t slow[] = {{3000, BIG}, {4100, BIG}};
    run(&s, 0, 0, 10000, slow, 2, false, 1, 0);
    assert(fires == 1);
    // ...and 1.5 s apart is two separate knocks
    fresh(&s);
    jolt_t toolate[] = {{3000, BIG}, {4500, BIG}};
    run(&s, 0, 0, 12000, toolate, 2, false, 1, 0);
    assert(fires == 0);

    // a vigorous back-and-forth (jolts 150 ms apart) is still one shake, once
    fresh(&s);
    jolt_t vigorous[6];
    for (int i = 0; i < 6; i++) vigorous[i] = (jolt_t){3000 + 150u * i, BIG};
    run(&s, 0, 0, 10000, vigorous, 6, false, 1, 0);
    assert(fires == 1);

    // one knock, however hard, never fires; nor does its ringing (many samples, one jolt)
    fresh(&s);
    jolt_t knock[] = {{3000, 3500}};
    run(&s, 0, 0, 10000, knock, 1, false, 1, 0);
    assert(fires == 0);
    fresh(&s);
    jolt_t ring[] = {{3000, 3500}, {3040, 2500}, {3080, 2600}};   // damped ring inside SHAKE_MIN_GAP_MS
    run(&s, 0, 0, 10000, ring, 3, false, 1, 0);
    assert(fires == 0);

    // picking the dial up: gravity turns 90 degrees over 1.5 s, magnitude stays 1 g
    fresh(&s);
    run(&s, 0, 0, 1500, NULL, 0, true, 1, 0);
    run(&s, 0, 1500, 6000, NULL, 0, false, 1, 0);
    assert(fires == 0);
    // ...with the lift's own bump on top (1.3 g), which is well under the jolt threshold
    fresh(&s);
    jolt_t lift[] = {{1000, 300}, {1400, 300}};
    run(&s, 0, 0, 6000, lift, 2, true, 1, 0);
    assert(fires == 0);

    // walking: 2 Hz strides. Weak strides (1.2 g peaks) are below the threshold...
    fresh(&s);
    jolt_t weak[40];
    for (int i = 0; i < 40; i++) weak[i] = (jolt_t){1000u + 500u * i, 1200};
    run(&s, 0, 0, 22000, weak, 40, false, 1, 0);
    assert(fires == 0);
    // ...and strong strides (well over it) never settle, so a stride train never fires either
    fresh(&s);
    jolt_t strong[40];
    for (int i = 0; i < 40; i++) strong[i] = (jolt_t){1000u + 500u * i, 2200};
    run(&s, 0, 0, 22000, strong, 40, false, 1, 0);
    assert(fires == 0);
    // slower strides: 580, 667 and 750 ms all stay under the settle time, so none of them ever settles
    static const uint32_t periods[] = {580, 667, 750};
    for (unsigned k = 0; k < sizeof periods / sizeof periods[0]; k++) {
        fresh(&s);
        for (int i = 0; i < 40; i++) strong[i] = (jolt_t){1000u + periods[k] * i, 2200};
        run(&s, 0, 0, 30000, strong, 40, false, 1, 0);
        assert(fires == 0);
    }
    // the limit of the rule, stated: a strong train slower than the settle time fires once (its second pulse)
    fresh(&s);
    for (int i = 0; i < 6; i++) strong[i] = (jolt_t){1000u + 1000u * i, 2200};
    run(&s, 0, 0, 10000, strong, 6, false, 1, 0);
    assert(fires == 1);

    // cooldown: a second shake 1.2 s after the first fired is swallowed; one after stillness fires
    fresh(&s);
    jolt_t again[] = {{3000, BIG}, {3400, BIG}, {4800, BIG}, {5200, BIG}, {12000, BIG}, {12400, BIG}};
    run(&s, 0, 0, 20000, again, 6, false, 1, 0);
    assert(fires == 2);
    assert(fired_at[0] < 4300 && fired_at[1] > 12400);

    // screen off: the shake counts for nothing, and the first moments after waking cannot arm it either
    fresh(&s);
    run(&s, 0, 0, 10000, two, 2, false, 2000, 9000);
    assert(fires == 0);
    fresh(&s);
    jolt_t wake[] = {{9200, BIG}, {9600, BIG}};   // screen came back at 9000: not yet still for SHAKE_QUIET_MS
    run(&s, 0, 0, 14000, wake, 2, false, 2000, 9000);
    assert(fires == 0);
    fresh(&s);
    jolt_t later[] = {{11000, BIG}, {11400, BIG}};
    run(&s, 0, 0, 16000, later, 2, false, 2000, 9000);
    assert(fires == 1);
    // going to sleep in the middle of a shake discards the half-counted group
    fresh(&s);
    jolt_t half[] = {{3000, BIG}, {3400, BIG}};
    run(&s, 0, 0, 10000, half, 2, false, 3450, 3450 + STEP);
    assert(fires == 0);

    // the clock wraps in the middle of a shake, and in the middle of the stillness before one
    for (uint32_t offset = 0; offset < 3000; offset += 250) {
        fresh(&s);
        jolt_t w[] = {{3000, BIG}, {3400, BIG}};
        run(&s, 0u - 3200u - offset, 0, 10000, w, 2, false, 1, 0);
        assert(fires == 1);
    }
    // a long idle across the wrap does not alias into a false stillness or a false group
    fresh(&s);
    jolt_t old[] = {{1000, BIG}};
    run(&s, 0xFFFFF000u, 0, 3000, old, 1, false, 1, 0);
    s.last_start = s.last_strong;   // as left behind
    ht_shake_feed(&s, 0xFFFFF000u + 3000u + (1u << 29), 0, 0, 1000, false);
    assert(!s.has_strong && !s.started);

    printf("shake: PASS (double, slow pair, vigorous, knock, ringing, tilt, lift, walking 2/1.5 Hz, cooldown, screen off, wrap); state=%zu bytes\n",
           sizeof s);
    return 0;
}
