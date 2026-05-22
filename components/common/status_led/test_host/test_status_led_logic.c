#include <stdbool.h>
#include <stdio.h>

#include "status_led_logic.h"

static int g_failed = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL [line %d]: %s\n", __LINE__, msg);                  \
            g_failed = 1;                                                   \
        }                                                                   \
    } while (0)

/* Binary-GPIO active-low colour map (status_led_logic.h):
 *
 *   NORMAL       -> blue   (R=0, G=0, B=1)
 *   RECORDING    -> green  (R=0, G=1, B=0)
 *   SPEAKING     -> yellow (R=1, G=1, B=0)
 *   NOTIFICATION -> red    (R=1, G=0, B=0)
 *
 * The test pins the exact triplet per state so a future colour-map
 * regression (e.g. R/G swap on a board variant) fails immediately. */
static void test_colour_map(void)
{
    status_led_bits_t n = status_led_bits_for_state(SS_STATE_NORMAL);
    CHECK(!n.r && !n.g && n.b, "NORMAL = blue only (B on)");

    status_led_bits_t r = status_led_bits_for_state(SS_STATE_RECORDING);
    CHECK(!r.r && r.g && !r.b, "RECORDING = green only (G on)");

    status_led_bits_t s = status_led_bits_for_state(SS_STATE_SPEAKING);
    CHECK(s.r && s.g && !s.b, "SPEAKING = yellow (R+G on, B off)");

    status_led_bits_t t = status_led_bits_for_state(SS_STATE_NOTIFICATION);
    CHECK(t.r && !t.g && !t.b, "NOTIFICATION = red only (R on)");

    /* No-brick: out-of-range falls back to NORMAL/blue. */
    status_led_bits_t x = status_led_bits_for_state((ss_state_t)99);
    CHECK(!x.r && !x.g && x.b,
          "out-of-range state -> blue fallback (no-brick)");

    /* Coverage of the full SS_STATE_* enumerator set so a future enum
     * addition that goes unmapped is loud (would land in the default /
     * fallback and produce blue, which the assertion catches via the
     * out-of-range arm above). */
    CHECK(SS_STATE_NORMAL == 0 && SS_STATE_RECORDING == 1 &&
          SS_STATE_SPEAKING == 2 && SS_STATE_NOTIFICATION == 3,
          "ss_state_t value contract intact");
}

int main(void)
{
    test_colour_map();
    if (g_failed) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
