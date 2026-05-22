# Agent Deck 1.0 — board adaptation

ESP Board Manager adaptation for Attaky's Agent Deck 1.0.

This directory holds the source-level configuration that ESP-IDF
needs to build firmware for Agent Deck 1.0 hardware:

- `board_info.yaml`, `board_devices.yaml`, `board_peripherals.yaml` —
  ESP Board Manager descriptors
- `sdkconfig.defaults.board` — board-specific sdkconfig overrides
- `setup_device.c` — board-specific initialization code
- `components/` — board-local custom drivers (if any)

Detailed pinmap, peripheral inventory, I2C address map, and bring-up
notes are maintained as Attaky-internal hardware documentation,
outside this repository.

Build the firmware as usual; ESP Board Manager auto-selects this
folder when the board target is `attaky/agent_deck_1_0`.
