/* setup_status_format.h
 *
 * Pure C99 string-formatting helpers for the Settings -> STATUS screen.
 * No ESP-IDF types: only <stdint.h> + <stddef.h>. The implementation
 * uses <stdio.h> for snprintf but the interface stays portable so the
 * helpers can be exercised from a plain host-cc test runner.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format a "Δ ago" string into `buf` per design §6.4 bucket rules:
 *   < 5 s      -> "just now"
 *   < 60 s     -> "Ns ago"   (floor seconds)
 *   < 3600 s   -> "Nm ago"   (floor minutes)
 *   < 86400 s  -> "Nh ago"   (floor hours)
 *   otherwise  -> "Nd ago"   (floor days)
 *
 * @param delta_us  Δ in microseconds. Negative input is clamped to 0
 *                  / "just now".
 * @param buf       caller-provided buffer. Must be non-NULL.
 * @param buflen    sizeof buf. Must be >= 12 to fit the widest
 *                  practical render ("Nd ago" / "Nm ago" with up to
 *                  ~999 days). Returns NULL if too small.
 * @return buf on success, NULL if buf is NULL, buflen < 12, or the
 *         render would overflow the buffer.
 */
char *format_call_age(int64_t delta_us, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
