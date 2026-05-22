/* setup_status_format.c — pure C99 implementation. See header for
 * the public contract and design §6.4 for the bucket rules. */
#include "setup_status_format.h"
#include <stdio.h>

char *format_call_age(int64_t delta_us, char *buf, size_t buflen)
{
    if (buf == NULL || buflen < 12) {
        return NULL;
    }
    if (delta_us < 0) delta_us = 0;
    int64_t s = delta_us / 1000000;
    int n;
    if (s < 5) {
        n = snprintf(buf, buflen, "just now");
    } else if (s < 60) {
        n = snprintf(buf, buflen, "%llds ago", (long long)s);
    } else if (s < 3600) {
        n = snprintf(buf, buflen, "%lldm ago", (long long)(s / 60));
    } else if (s < 86400) {
        n = snprintf(buf, buflen, "%lldh ago", (long long)(s / 3600));
    } else {
        n = snprintf(buf, buflen, "%lldd ago", (long long)(s / 86400));
    }
    if (n < 0 || (size_t)n >= buflen) {
        return NULL;
    }
    return buf;
}
