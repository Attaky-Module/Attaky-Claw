# Lua Touch

This module describes how to read capacitive touch key data from Lua.
When a request mentions `touch`, `touch keys`, or `capacitive sensor`, use this module by default.

## How to call
- Import it with `local touch = require("touch")`
- Open the device with one of:
  - `local keys = touch.new()` — use board defaults from the device named `touch_keys`
  - `local keys = touch.new("touch_keys")` — choose a device name explicitly
  - `local keys = touch.new({ key1_gpio = 2, key2_gpio = 3 })` — provide GPIOs directly
  - `local keys = touch.new("touch_keys", { threshold_milli = 20 })` — board defaults + overrides
- Call `local sample = keys:read()` to get all key states
- Call `local pressed = keys:is_pressed(index)` to check one key (1-based index)
- Call `keys:name()` to get the device name string
- Call `keys:close()` when done

## Options table
All fields are optional. Any field omitted falls back to the board `board_devices.yaml` value for the device named by `device` (default `"touch_keys"`).

| Field             | Type    | Meaning                                              |
|-------------------|---------|------------------------------------------------------|
| `device`          | string  | Board device name to read defaults from              |
| `key1_gpio`       | integer | GPIO number of the first touch key                   |
| `key2_gpio`       | integer | GPIO number of the second touch key (optional)       |
| `threshold_milli` | integer | Active threshold in permille of benchmark (default from Kconfig) |

## Data format
`keys:read()` returns a table with:
- `sample.keys` — array of key tables, each containing:
  - `key.index` — 1-based key index
  - `key.channel` — touch sensor channel number
  - `key.gpio` — GPIO number
  - `key.pressed` — boolean
  - `key.smooth` — smoothed raw sensor value
  - `key.benchmark` — baseline reference value
  - `key.delta` — difference between smooth and benchmark
  - `key.threshold` — active threshold value
- `sample.count` — total number of keys
- `sample.any_pressed` — boolean, true if any key is pressed
- `sample.pressed_count` — number of currently pressed keys

## Example
```lua
local touch = require("touch")

local keys = touch.new()                  -- uses board defaults
local sample = keys:read()
for _, key in ipairs(sample.keys) do
    print(key.index, key.pressed, key.delta)
end
keys:close()
```
