#!/usr/bin/env python3
# Companion dev tool for the emote debug console
# (CONFIG_APP_EMOTE_DEBUG_CONSOLE):
# Cycle the device through each visually-distinct packed emote over the
# serial console so the on-device layout can be eyeballed per expression.
#
# Usage:
#   <espressif-python> tools/emote_cycle.py [--port P] [--hold S] [--once]
#
# Default: /dev/cu.usbserial-2120 @115200, 10 s per emote, loop forever
# until Ctrl-C. Each step prints the emote name + the device's ESP_xxx
# return so the printed name always matches what is on screen.

import argparse
import sys
import time

import serial  # pyserial (present in the espressif python env)

# Visually-distinct emotes (deduplicated by underlying .eaf in
# assets_local/320_240/emote.json). Names are the logical "emote" keys,
# not the .eaf filenames.
EMOTES = [
    "happy",     # Happy.eaf   (also laughing/funny/loving/surprised/...)
    "sad",       # Sad.eaf
    "crying",    # cry.eaf
    "sleepy",    # sleep.eaf
    "angry",     # angry.eaf
    "shocked",   # shocked.eaf
    "confused",  # confused.eaf (== "thinking")
    "winking",   # neutral.eaf  (upstream emote.json src swap)
    "neutral",   # winking.eaf  (upstream emote.json src swap)
]


def reset_and_drain(p, boot_secs=7.0):
    """Pulse DTR/RTS to reset the ESP, then drain the boot log."""
    p.setDTR(False)
    p.setRTS(True)
    time.sleep(0.1)
    p.setRTS(False)
    time.sleep(0.05)
    p.reset_input_buffer()
    t = time.time()
    while time.time() - t < boot_secs:
        p.read(1024)
    p.write(b"\r\n")
    time.sleep(0.4)
    p.read(4096)


def show(p, name, hold):
    p.reset_input_buffer()
    p.write(("emote %s\r\n" % name).encode())
    time.sleep(1.2)
    raw = p.read(4096).decode("utf-8", "replace")
    rc = next(
        (ln.strip() for ln in raw.splitlines() if "emote_debug_show" in ln),
        "(no ack)",
    )
    stamp = time.strftime("%H:%M:%S")
    print("[%s] emote %-9s | %s" % (stamp, name, rc), flush=True)
    remaining = hold - 1.2
    if remaining > 0:
        time.sleep(remaining)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-2120")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--hold", type=float, default=10.0,
                    help="seconds to hold each emote (default 10)")
    ap.add_argument("--once", action="store_true",
                    help="single pass then exit (default: loop forever)")
    args = ap.parse_args()

    p = serial.Serial(args.port, args.baud, timeout=0.2)
    print("Opened %s @%d -- resetting device..." % (args.port, args.baud),
          flush=True)
    reset_and_drain(p)
    print("Boot drained. Cycling %d emotes, %.0fs each. Ctrl-C to stop.\n"
          % (len(EMOTES), args.hold), flush=True)

    try:
        loop = 0
        while True:
            loop += 1
            print("--- pass %d ---" % loop, flush=True)
            for name in EMOTES:
                show(p, name, args.hold)
            if args.once:
                break
    except KeyboardInterrupt:
        print("\nStopped by user.", flush=True)
    finally:
        p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
