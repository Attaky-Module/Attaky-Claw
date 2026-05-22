# EAF Format Notes (derived from the in-tree decoder)

Authoritative source: the in-tree decoder is the format spec. All
citations below are line numbers in
`application/edge_agent/managed_components/espressif2022__esp_emote_gfx/src/lib/eaf/gfx_eaf_dec.c`
unless prefixed otherwise:

- `gfx_eaf_dec.c` -- decoder implementation (this analysis's primary source)
- `gfx_eaf_dec.h` -- `application/edge_agent/managed_components/espressif2022__esp_emote_gfx/src/lib/eaf/gfx_eaf_dec.h`
  (constants, structs, enums)
- `gfx_anim.c` -- `application/edge_agent/managed_components/espressif2022__esp_emote_gfx/src/widget/gfx_anim.c`
  (the only consumer of the decoder; constrains which format paths actually work)

Build config that gates decoders is `application/edge_agent/sdkconfig`:

- `CONFIG_GFX_EAF_JPEG_DECODE_SUPPORT=y` (line 2405)
- `CONFIG_GFX_EAF_HEATSHRINK_SUPPORT=y` (line 2406)
- `CONFIG_HEATSHRINK_DYNAMIC_ALLOC=y` (line 2454)
- `CONFIG_HEATSHRINK_USE_INDEX=y` (line 2456)

All multi-byte integers are read with little-endian host pointer casts
(e.g. `*(uint16_t *)(file_data + 10)`, `gfx_eaf_dec.c:168`). The ESP32
target is little-endian, so **every multi-byte field documented below is
little-endian**. The encoder must emit little-endian.

---

## 1. Container layout (`EAF` / `AAF` file)

Parsed by `eaf_init()` (`gfx_eaf_dec.c:705-765`). Offset constants from
`gfx_eaf_dec.h:48-60`:

| Const | Value | Meaning |
|---|---|---|
| `EAF_FORMAT_OFFSET` | 0 | magic byte offset |
| `EAF_STR_OFFSET` | 1 | format string offset |
| `EAF_NUM_OFFSET` | 4 | total frame count offset |
| `EAF_CHECKSUM_OFFSET` | 8 | checksum offset |
| `EAF_TABLE_LEN` | 12 | length-of-(table+data) offset |
| `EAF_TABLE_OFFSET` | 16 | frame table offset |
| `EAF_FORMAT_MAGIC` | `0x89` | magic byte value |
| `EAF_FORMAT_STR` | `"EAF"` | 3-byte format string |
| `AAF_FORMAT_STR` | `"AAF"` | accepted alternative 3-byte string |
| `EAF_MAGIC_HEAD` | `0x5A5A` | per-frame 2-byte magic header |
| `EAF_MAGIC_LEN` | 2 | per-frame magic length |

### Container header (bytes 0..15)

| Offset | Size | Field | Decoder reference |
|---|---|---|---|
| 0 | 1 | magic = `0x89`; checked `data[EAF_FORMAT_OFFSET] == EAF_FORMAT_MAGIC` | `gfx_eaf_dec.c:724` |
| 1 | 3 | format string; `memcmp(...,"EAF",3)==0 \|\| memcmp(...,"AAF",3)==0` | `gfx_eaf_dec.c:726-728` |
| 4 | 4 | `total_frames`, read as `*(int *)` | `gfx_eaf_dec.c:730` |
| 8 | 4 | `stored_chk` (`uint32_t`) container checksum | `gfx_eaf_dec.c:731` |
| 12 | 4 | `stored_len` (`uint32_t`) length of the checksummed region | `gfx_eaf_dec.c:732` |

Note byte 1..3 is the format string only; byte 0 is the magic. The
header comment block at `gfx_eaf_dec.h:34-45` says "0:1 magic, 1:3 string,
4:4 frames, 8:4 checksum, 12:4 length, 16:N table, 16+N:M data" -- code
matches this.

`total_frames` is read with `*(int *)` (signed 32-bit, line 730).
Emit it as a 32-bit little-endian value; for valid frame counts the
sign bit is unused so it is equivalent to `uint32`.

### Checksum (`eaf_calculate_checksum`, `gfx_eaf_dec.c:24-31`)

```c
static uint32_t eaf_calculate_checksum(const uint8_t *data, uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) checksum += data[i];
    return checksum;
}
```

- Plain unsigned 32-bit **sum of bytes** (wraps mod 2^32). Not CRC.
- Computed over `data + EAF_TABLE_OFFSET` for `stored_len` bytes
  (`gfx_eaf_dec.c:734`): i.e. checksum covers the frame table **plus**
  all frame data, starting at byte 16.
- Validity gate: `calculated_chk == stored_chk`
  (`gfx_eaf_dec.c:735`), else `ESP_ERR_INVALID_CRC`.
- Encoder rule: write everything from offset 16 onward first, set
  `stored_len = (total file size) - 16`, set
  `stored_chk = sum of every byte from offset 16 to EOF`.

### Frame table (`eaf_frame_table_entry_t`, `gfx_eaf_dec.h:69-74`)

`#pragma pack(1)` struct, 8 bytes each:

```c
#pragma pack(1)
typedef struct {
    uint32_t frame_size;    // offset +0 within entry
    uint32_t frame_offset;  // offset +4 within entry
} eaf_frame_table_entry_t;
#pragma pack()
```

- Table starts at file offset `EAF_TABLE_OFFSET` = 16.
- `total_frames` entries, 8 bytes each -> table size = `total_frames * 8`.
- Frame data region starts immediately after the table:
  `EAF_TABLE_OFFSET + total_frames * sizeof(eaf_frame_table_entry_t)`
  (`gfx_eaf_dec.c:742`).
- For frame `i`, the frame blob lives at
  `data + EAF_TABLE_OFFSET + total_frames*8 + table[i].frame_offset`
  (`gfx_eaf_dec.c:742`). So `frame_offset` is **relative to the start
  of the frame-data region**, not absolute.
- `frame_size` (`table[i].frame_size`) is the **total stored size of
  that frame blob including its 2-byte magic header** -- see
  `eaf_get_frame_size()` returning `frame_size - EAF_MAGIC_LEN`
  (`gfx_eaf_dec.c:821`).

### Per-frame magic header

- Each frame blob begins with a 2-byte magic `uint16_t == EAF_MAGIC_HEAD`
  = `0x5A5A` (`gfx_eaf_dec.c:744-745`). Little-endian, so the two bytes
  on disk are `0x5A 0x5A` (palindromic -- endianness irrelevant here).
- `eaf_get_frame_data()` returns `frame_mem + EAF_MAGIC_LEN`
  (`gfx_eaf_dec.c:804`): the frame-level parser (`_S`/`_C`) sees data
  **after** the 2-byte magic.
- `eaf_get_frame_size()` returns `table->frame_size - EAF_MAGIC_LEN`
  (`gfx_eaf_dec.c:821`): logical frame payload size excludes the magic.

So each frame blob on disk = `[0x5A 0x5A]` + `[ _S/_C frame stream ]`,
and `frame_size` in the table = `2 + len(_S/_C stream)`.

---

## 2. Frame stream: `_S` (static image) vs `_C`

Parsed by `eaf_get_frame_info()` (`gfx_eaf_dec.c:185-259`); a lighter
validity probe is `eaf_probe_frame_info()` (`gfx_eaf_dec.c:133-183`).
The pointer passed in is `eaf_get_frame_data()` output = blob + 2
(past the `0x5A5A` magic).

- First 2 bytes = `format[2]` (`gfx_eaf_dec.c:206-207`); a NUL is
  appended in-struct only.
- `strncmp(format,"_S",2)==0` -> static image frame, returns
  `EAF_FORMAT_VALID` (`gfx_eaf_dec.c:209`, `:251`).
- `strncmp(format,"_C",2)==0` -> returns `EAF_FORMAT_FLAG`
  (`gfx_eaf_dec.c:253-254`; identical in probe at `:176-177`).
- anything else -> `EAF_FORMAT_INVALID` (`gfx_eaf_dec.c:255-258`).

### `_C` role -- OPEN QUESTION (resolved enough to avoid it)

`eaf_get_frame_info()`/`eaf_probe_frame_info()` only return
`EAF_FORMAT_FLAG` for `_C`; the decoder **never parses any `_C` body**.
The sole consumer `gfx_anim.c:234-235` treats `EAF_FORMAT_FLAG` as a
hard `return ESP_FAIL` (frame not rendered). `EAF_FORMAT_REDIRECT`
(`gfx_eaf_dec.h:101`) is declared but **never produced** by the
decoder.

**OPEN QUESTION (non-blocking):** the internal byte layout of a `_C`
frame is *not determinable from this decoder* -- it is recognized only
by its 2-byte `"_C"` tag and then rejected by the consumer. The plan's
task description references a "`_C` container layout (frame count,
per-frame offset/size index, checksum)"; in *this* codebase that
description matches the **container header + frame table** of section 1
(magic `0x89`, `"EAF"`, frame count @4, checksum @8, the
size/offset frame table @16), **not** a `_C` frame body. There is no
evidence in `gfx_eaf_dec.c` of a separate `_C`-bodied multi-frame index
structure. **Encoder decision: never emit `_C`.** Emit `_S` frames
only. This is safe because the renderer rejects `_C` outright
(`gfx_anim.c:234`). Flagging rather than guessing per the task's
ambiguity rule; the next task validates via host round-trip.

### `_S` static frame header

Field reads in `eaf_get_frame_info()` (`gfx_eaf_dec.c:206-250`).
Offsets are relative to the start of the `_S` stream (= frame blob
byte 2, i.e. just past `0x5A5A`):

| Offset | Size | Field | Decoder reference |
|---|---|---|---|
| 0 | 2 | `format` = ASCII `"_S"` | `gfx_eaf_dec.c:206-209` |
| 2 | 1 | gap byte; **not read** (version copied from +3). Set 0. | (see note) |
| 3 | 6 | `version[6]`, `memcpy(...,file_data+3,6)` -- value unchecked | `gfx_eaf_dec.c:210` |
| 9 | 1 | `bit_depth`; must be 4, 8, or 24 | `gfx_eaf_dec.c:212-217` |
| 10 | 2 | `width` (`uint16`) | `gfx_eaf_dec.c:219` |
| 12 | 2 | `height` (`uint16`) | `gfx_eaf_dec.c:221` |
| 14 | 2 | `blocks` (`uint16`) | `gfx_eaf_dec.c:221` |
| 16 | 2 | `block_height` (`uint16`) | `gfx_eaf_dec.c:222` |
| 18 | `blocks*4` | block length table: `blocks` x `uint32` | `gfx_eaf_dec.c:230-232` |
| 18 + blocks*4 | `num_colors*4` | palette (omitted if bit_depth==24) | `gfx_eaf_dec.c:248` |
| `data_offset` | ... | block payload region | `gfx_eaf_dec.c:250` |

Notes:

- Byte at offset 2 of the `_S` stream is never read; `format` is 2
  bytes (`gfx_eaf_dec.c:206`), version starts at +3 (`:210`). Offset 2
  is an unused separator -- emit `0x00`. (OPEN QUESTION, minor: its
  intended semantics -- likely a NUL terminator after `"_S"` -- are not
  observable from the decoder; any value works. Validate in round-trip.)
- `eaf_probe_frame_info()` additionally rejects the frame if any of
  `width/height/blocks/block_height == 0` (`gfx_eaf_dec.c:173-175`),
  and rejects bad bit depths (`:163-166`). The encoder must keep all
  four of those non-zero.
- `block_len[i] = *(uint32_t *)(file_data + 18 + i*4)`
  (`gfx_eaf_dec.c:231`): each block's **stored byte length including
  the 1-byte encoding tag** (see section 4 / section 5). Little-endian `uint32`.
- `num_colors = 1 << bit_depth` (`gfx_eaf_dec.c:234`). For
  `bit_depth==8` -> `num_colors = 256`. For `bit_depth==24`,
  `num_colors=0`, `palette=NULL` (`:236-238`). NOTE: for
  `bit_depth==4`, `1<<4 = 16` (not 256) -- but 4-bit is not rendered
  (see section 3), so irrelevant for our encoder.
- Palette is `num_colors * 4` bytes, `memcpy` from
  `file_data + 18 + blocks*4` (`gfx_eaf_dec.c:240,248`).
- `data_offset = 18 + blocks*4 + num_colors*4`
  (`gfx_eaf_dec.c:250`). It is a `uint16_t` in `eaf_header_t`
  (`gfx_eaf_dec.h:131`) -- keep header+table+palette under 64 KiB.

### Block tiling / layout (horizontal full-width strips stacked vertically)

From `eaf_calculate_offsets()` (`gfx_eaf_dec.c:273-279`) and
`eaf_frame_decode()` (`gfx_eaf_dec.c:828-923`):

- `offsets[0] = data_offset`;
  `offsets[i] = offsets[i-1] + block_len[i-1]`
  (`gfx_eaf_dec.c:275-278`). Blocks are stored **contiguously** in
  declared order starting at `data_offset`; `block_len[i]` is the
  exact stored size of block `i` (tag byte included).
- Decoded block uncompressed size = `width * block_height` bytes for
  bit_depth 8 (one index byte per pixel); `eaf_decode_block` sets
  `out_size = width * block_height` (`gfx_eaf_dec.c:394`, non-JPEG
  branch). For JPEG, `out_size = width*block_height*2`
  (`gfx_eaf_dec.c:392`). For 24-bit, block_size doubles
  (`gfx_eaf_dec.c:855`).
- Each block is a horizontal full-width strip of `block_height` rows.
  Block `b` covers rows `[b*block_height, (b+1)*block_height)`.
- Last block may be partial: `eaf_frame_decode` clamps
  `valid_size = (height - block*block_height) * width` when
  `(block+1)*block_height > height` (`gfx_eaf_dec.c:889-893`). The
  decoded buffer is still sized `width*block_height`, but only the
  valid rows are converted to RGB565. **Encoder rule:** every block's
  *decoded* payload must be exactly `width*block_height` index bytes
  (full strip, including padding rows in the final block), even though
  only `valid_size` of them are used. `block_height` need not divide
  `height`; pad the final strip's extra rows with any palette index
  (index 0 recommended).
- `blocks` must satisfy `blocks * block_height >= height` to cover the
  whole image. Pixel row-major, top-to-bottom, left-to-right within a
  strip (index `i` in the decoded block buffer = pixel at
  `row = i / width`, `col = i % width` within that strip;
  `gfx_eaf_dec.c:897-910`).

---

## 3. `EAF_ENCODING_*` enum and registered decoders

Enum (`gfx_eaf_dec.h:109-117`):

| Name | Value | Meaning |
|---|---|---|
| `EAF_ENCODING_RLE` | 0 | Run-length encoding |
| `EAF_ENCODING_HUFFMAN` | 1 | Huffman + RLE |
| `EAF_ENCODING_JPEG` | 2 | JPEG |
| `EAF_ENCODING_HUFFMAN_DIRECT` | 3 | Huffman, no RLE |
| `EAF_ENCODING_HEATSHRINK` | 4 | Heatshrink (LZSS) |
| `EAF_ENCODING_RAW` | 5 | Raw / uncompressed (stored) |
| `EAF_ENCODING_MAX` | 6 | count (not a real encoding) |

Registration in `eaf_init_decoders()` (`gfx_eaf_dec.c:351-367`):

| Encoding | Registered? in THIS build | Decoder fn | Citation |
|---|---|---|---|
| RLE (0) | yes (unconditional) | `eaf_decode_rle` | `gfx_eaf_dec.c:355` |
| HUFFMAN (1) | yes (unconditional) | `eaf_decode_huffman_rle` | `gfx_eaf_dec.c:356` |
| HUFFMAN_DIRECT (3) | yes (unconditional) | `eaf_decode_huffman` | `gfx_eaf_dec.c:357` |
| JPEG (2) | **yes** -- `CONFIG_GFX_EAF_JPEG_DECODE_SUPPORT=y` | `eaf_decode_jpeg` | `gfx_eaf_dec.c:358-360` |
| HEATSHRINK (4) | **yes** -- `CONFIG_GFX_EAF_HEATSHRINK_SUPPORT=y` | `eaf_decode_heatshrink` | `gfx_eaf_dec.c:361-363` |
| RAW (5) | yes (unconditional) | `eaf_decode_raw` | `gfx_eaf_dec.c:364` |

All six encodings are registered and usable in this specific build
(both gating Kconfigs are `=y` in `application/edge_agent/sdkconfig`).

`eaf_decode_block()` (`gfx_eaf_dec.c:369-407`):

- `encoding_type = block_data[0]` -- the **first byte of each block is
  the encoding tag** (`gfx_eaf_dec.c:372`).
- Bounds-checked against `EAF_ENCODING_MAX` array size
  (`gfx_eaf_dec.c:378`); rejected if no decoder registered
  (`gfx_eaf_dec.c:384-387`).
- Decoder is called with `block_data + 1`, `block_len - 1`
  (`gfx_eaf_dec.c:400`): the tag byte is stripped; the rest is the
  encoded payload.

Bit-depth handling in `eaf_frame_decode()`:

- `bit_depth == 8`: each decoded byte is a palette index; mapped to
  RGB565 via `eaf_palette_get_color` with a 256-entry cache
  (`gfx_eaf_dec.c:896-910`).
- `bit_depth == 4`: **`ESP_LOGW("4-bit depth not supported")` -- not
  rendered** (`gfx_eaf_dec.c:911-912`). Do not use.
- `bit_depth == 24`: decoded bytes are RGB565 pairs `memcpy`'d
  directly (`gfx_eaf_dec.c:913-914`); 2 bytes/pixel.

---

## 4. Chosen encoding and bit depth

### Default block encoding: `EAF_ENCODING_RLE` (tag byte `0x00`)

`EAF_ENCODING_RLE` is the **default** the encoder emits, and the
encoding every shipping animation asset (`listen.eaf`, `neutral.eaf`)
uses. It is registered **unconditionally**
(`eaf_init_decoders` -> `eaf_register_decoder(EAF_ENCODING_RLE,
eaf_decode_rle)`, `gfx_eaf_dec.c:355`; no Kconfig gate), so it works in
every build. A breathing status disc is a huge flat opaque-black
background plus a small radial blob, which RLE compresses massively
(measured: a mostly-black 34x34x4 frameset is **~52% smaller** as RLE
vs RAW), letting `status_*.eaf` fit the emote partition once the
hardware-visual gate's frames/canvas are added. RAW (next subsection)
is retained and still valid; select it with `encoding=EAF_ENCODING_RAW`.

#### `eaf_decode_rle` is the authoritative RLE oracle (`gfx_eaf_dec.c:409-443`)

```c
esp_err_t eaf_decode_rle(const uint8_t *input_data, size_t input_size,
                         uint8_t *output_buffer, size_t *out_size,
                         bool swap_color) {
    (void)swap_color;                                  // :413 unused
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos + 1 < input_size) {                  // :418
        uint8_t repeat_count = input_data[in_pos++];   // :419
        uint8_t repeat_value = input_data[in_pos++];   // :420
        if (out_pos + repeat_count > *out_size) {      // :422
            ESP_LOGE(...);                              // :423
            return ESP_FAIL;                            // :424
        }
        uint32_t value_4bytes = repeat_value | (repeat_value<<8)
                              | (repeat_value<<16) | (repeat_value<<24);
        while (repeat_count >= 4) {                     // :428
            *((uint32_t*)(output_buffer+out_pos)) = value_4bytes; // :430
            out_pos += 4; repeat_count -= 4;            // :431-432
        }
        while (repeat_count > 0) {                      // :435
            output_buffer[out_pos++] = repeat_value;    // :436
            repeat_count--;                             // :437
        }
    }
    *out_size = out_pos;                                // :441
    return ESP_OK;                                      // :442
}
```

Exact RLE block byte layout the decoder requires (bit_depth = 8):

```
byte 0                : 0x00            (EAF_ENCODING_RLE tag)
bytes 1 ..            : (count, value) pairs, count first; each pair
                        expands to `count` copies of `value`. The pair
                        stream has even length; concatenated expansions
                        equal exactly width*block_height index bytes.
```

Every rule, cited to the C source (the format oracle):

- **Tag stripped before the RLE decoder runs.** `eaf_decode_block`
  reads the tag at `block_data[0]` (`gfx_eaf_dec.c:372`) and calls the
  registered decoder with `block_data + 1, block_len - 1`
  (`gfx_eaf_dec.c:400`) -- identical to the RAW dispatch; only the
  callback differs (`:355` vs `:364`). So the `0x00` tag is **not**
  part of the pair stream the loop above sees; `input_size` =
  `block_len - 1`.
- **Pair order is `(count, value)`, both `uint8`.**
  `repeat_count = input_data[in_pos++]` then
  `repeat_value = input_data[in_pos++]` (`gfx_eaf_dec.c:419-420`).
- **Max run per pair = 255.** `repeat_count` is a `uint8`
  (`gfx_eaf_dec.c:419`); a run longer than 255 **must be split** into
  consecutive same-`value` pairs each <= 255 (e.g. 600 -> `255,255,90`).
  The decoder simply appends each pair's expansion
  (`gfx_eaf_dec.c:427-438`), so split runs reconstruct bit-identically.
- **`count == 0` is a valid no-op pair** (the `:428`/`:435` write loops
  do not execute; the `:422` bounds check `out_pos + 0 > *out_size` is
  harmless). The encoder never emits one (every run length >= 1) so the
  stream is minimal, but the decoder tolerates it.
- **The 4-byte fast path + byte tail (`gfx_eaf_dec.c:427-438`) together
  write exactly `count` copies of `value`** -- semantically a plain run;
  the 4-wide unrolled store is only a speed optimisation.
- **Output size is fixed at `width*block_height`.** `eaf_decode_block`
  sets `out_size = width * block_height` (non-JPEG branch,
  `gfx_eaf_dec.c:394`) before calling the decoder. The overflow gate
  `out_pos + repeat_count > *out_size` (`gfx_eaf_dec.c:422-425`) fails
  the whole decode if the pairs ever over-produce. **Encoder rule:** the
  pair expansions must sum to *exactly* `width*block_height` (the full
  padded strip, exactly like RAW). The final partial strip is still a
  full `width*block_height` payload (pad surplus rows with index 0);
  the renderer's `valid_size` clamp (`gfx_eaf_dec.c:889-893`) then
  ignores the padding rows -- same as RAW.
- **Loop guard drops a lone trailing byte.**
  `while (in_pos + 1 < input_size)` (`gfx_eaf_dec.c:418`) requires >= 2
  bytes to read a pair, so a stray final byte is silently discarded.
  **Encoder rule:** always emit an even-length pair stream (it always
  is -- one pair per run-chunk) so no data is lost.
- `*out_size = out_pos` (`gfx_eaf_dec.c:441`): decoded length is
  whatever was produced (= `width*block_height` when the encoder
  obeys the rule above). `swap_color` is unused (`:413`).

Stored block length recorded in the `_S` block-length table
(`block_len[i]`, section 2) = `1 + len(pair_stream)` (tag + pairs). **Unlike
RAW this is *not* constant across blocks** -- it varies with how well
each strip compresses. The encoder must compute and store each block's
real stored length; `eaf_calculate_offsets` (`gfx_eaf_dec.c:273-279`)
then chains the contiguous blocks by those lengths exactly as for RAW.

### Retained block encoding: `EAF_ENCODING_RAW` (tag byte `0x05`)

Rationale: an uncompressed/stored encoding exists; retained as a valid,
selectable alternative (`encoding=EAF_ENCODING_RAW`) and used by the
existing round-trip cases. `eaf_decode_raw()` (`gfx_eaf_dec.c:445-466`):

```c
esp_err_t eaf_decode_raw(in, in_size, out, out_size, swap) {
    if (!in || !out || !out_size) return ESP_FAIL;
    if (*out_size < input_size) return ESP_ERR_INVALID_SIZE; // out buf too small
    if (input_size > 0) memcpy(out, in, input_size);
    *out_size = input_size;
    return ESP_OK;
}
```

- It is a pure `memcpy` of the post-tag payload into the decode buffer.
- `*out_size` enters as `width * block_height` (set by
  `eaf_decode_block`, `gfx_eaf_dec.c:394`). The guard
  `*out_size < input_size` (`gfx_eaf_dec.c:456`) means the RAW payload
  length **must be <= `width*block_height`**. To make the full strip
  valid we set it **exactly equal**: `input_size == width*block_height`.
- It does not touch `swap_color` and is unconditionally registered
  (`gfx_eaf_dec.c:364`). No Kconfig dependency, no compression
  libraries -- simplest possible, byte-exact.

#### Exact RAW block byte layout (bit_depth = 8)

For a block that is a `width x block_height` strip of 8-bit palette
indices:

```
byte 0                : 0x05            (EAF_ENCODING_RAW tag)
bytes 1 .. W*Bh       : W*Bh palette-index bytes, row-major
                        (row r, col c -> byte index r*W + c, 0-based;
                         r in [0,Bh), c in [0,W))
```

- Stored block length recorded in the `_S` block-length table
  (`block_len[i]`, section 2) = `1 + width*block_height` (tag + payload).
- The final (possibly partial) strip is still a full
  `width*block_height` payload; pad unused trailing rows with index 0
  (decoder ignores them via `valid_size`, section 2).
- `eaf_calculate_offsets` requires blocks be concatenated in order at
  `data_offset` (section 2); with RAW every block is
  `1 + width*block_height` bytes, so offsets are trivially derived.

Single-block option: setting `blocks = 1`, `block_height = height`
yields one RAW block of `1 + width*height` bytes. Allowed
(`block_height` need not be < height; only the `>0` and coverage
constraints apply). Simplest for a small status circle. Multi-block is
also fine and uses the same per-block layout.

### Chosen bit depth: 8-bit indexed (256-colour palette)

`bit_depth = 8` (byte at `_S`+9). `num_colors = 1<<8 = 256`
(`gfx_eaf_dec.c:234`). Decoder maps each index byte -> RGB565 via the
palette + cache (`gfx_eaf_dec.c:896-910`). This is the only
palette-indexed depth the renderer supports (4-bit is explicitly
unsupported, `gfx_eaf_dec.c:911-912`; 24-bit skips the palette).

#### Palette layout exactly as the decoder reads it

Palette block: `256 * 4 = 1024` bytes, stored at
`_S offset 18 + blocks*4`, `memcpy`'d whole
(`gfx_eaf_dec.c:240,248`). Each entry is **4 bytes**. Indexed by the
pixel byte: `color_data = &palette[color_index * 4]`
(`gfx_eaf_dec.c:287`).

Per-entry byte meaning, from `eaf_palette_get_color()`
(`gfx_eaf_dec.c:285-303`):

```c
const uint8_t *c = &palette[color_index * 4];
if (c[0]==0 && c[1]==0 && c[2]==0 && c[3]==0) return true; // fully transparent
uint16_t rgb565 = ((c[2] & 0xF8) << 8)   // Red   from c[2]
                | ((c[1] & 0xFC) << 3)   // Green from c[1]
                | ((c[0] & 0xF8) >> 3);  // Blue  from c[0]
// if swap_bytes: rgb565 = __builtin_bswap16(rgb565)
result->full = rgb565;
```

So each 4-byte palette entry is **`[B, G, R, A]`**:

| Entry byte | Role |
|---|---|
| `c[0]` | **Blue** (8-bit); contributes `(B & 0xF8) >> 3` -> RGB565 bits 0-4 |
| `c[1]` | **Green** (8-bit); contributes `(G & 0xFC) << 3` -> RGB565 bits 5-10 |
| `c[2]` | **Red** (8-bit); contributes `(R & 0xF8) << 8` -> RGB565 bits 11-15 |
| `c[3]` | Alpha / sentinel byte |

Transparency rule: an entry is treated as **fully transparent iff all
four bytes are 0** (`c[0]==c[1]==c[2]==c[3]==0`,
`gfx_eaf_dec.c:290-292`). `eaf_palette_get_color` returns `true`
(transparent) and leaves `result->full` **unwritten**. In
`eaf_frame_decode` the transparent case still stores whatever
`eaf_color.full` held (uninitialized `gfx_color_t eaf_color;`,
`gfx_eaf_dec.c:902-904`) into the cache and pixel -- i.e. transparency
is **not actually alpha-composited here**; the boolean return is
ignored at `gfx_eaf_dec.c:903`.

**OPEN QUESTION (non-blocking):** the `bool` transparent return of
`eaf_palette_get_color` is discarded in `eaf_frame_decode`
(`gfx_eaf_dec.c:903` ignores it; `eaf_color` is uninitialized when
transparent). Real alpha handling likely lives in the blit/compositor
(`gfx_anim.c` / object draw), out of scope for the format. **Encoder
rule to stay safe and deterministic:** never emit the
all-zero `[0,0,0,0]` entry for a colour we actually want drawn; if a
truly opaque black is needed use e.g. `B=0,G=0,R=0,A=0xFF`
(`[0x00,0x00,0x00,0xFF]`) so the all-zero transparent test fails and a
defined RGB565 (0x0000) is produced. The next task's host round-trip
should confirm exact on-device pixel values.

Non-transparent -> RGB565 quantisation the encoder must mirror:

```
R5 = (R8 & 0xF8) >> 3   (top 5 bits of red)
G6 = (G8 & 0xFC) >> 2   (top 6 bits of green)
B5 = (B8 & 0xF8) >> 3   (top 5 bits of blue)
rgb565 = (R5 << 11) | (G6 << 5) | B5
```
(equivalently the exact expression at `gfx_eaf_dec.c:299-300`). The
low bits of each channel are discarded; encoder need not pre-quantise
but the resulting on-screen colour is the 565-truncated value.

`swap_bytes` is passed by the caller (`gfx_anim.c`). When true the
final `uint16` is byte-swapped (`gfx_eaf_dec.c:299`,
`__builtin_bswap16`). This is a *display endianness* concern handled at
decode time and is **independent of the encoded file** -- the encoder
stores 8-bit B/G/R/A palette bytes regardless; no encoder action
needed.

---

## 5. Summary: byte-exact `_S` + RLE/RAW + 8-bit encoder recipe

The container / `_S` header / palette / frame table / checksum are
**identical** for both encodings. Only the per-block payload and the
per-block stored length in the block-length table differ.

Per animation file (`EAF`):

1. **Container header (16 B):** `0x89`, `"EAF"`, `total_frames`
   (u32 LE), `stored_chk` (u32 LE, fill after), `stored_len` (u32 LE,
   = filesize - 16).
2. **Frame table:** `total_frames` x 8 B, each `{frame_size u32 LE,
   frame_offset u32 LE}`. `frame_offset` relative to start of
   frame-data region (= 16 + total_frames*8). `frame_size` =
   `2 + len(_S stream)` (includes the 0x5A5A magic).
3. **Each frame blob:** `0x5A 0x5A` then the `_S` stream:
   - `"_S"` (2 B) | `0x00` separator (1 B) | `version` (6 B, e.g.
     zeros) | `bit_depth=0x08` (1 B) | `width` u16 LE (1 B at off 10) |
     `height` u16 LE | `blocks` u16 LE | `block_height` u16 LE.
   - block-length table: `blocks` x u32 LE; each = the EXACT stored
     size of that block including its 1-byte tag.
     - **RAW:** constant `1 + width*block_height`.
     - **RLE:** `1 + len(pair_stream)` -- varies per block with how
       well that strip compresses.
   - palette: 256 x 4 B, entry = `[B, G, R, A]`; avoid `[0,0,0,0]`
     for any drawn colour (use A=0xFF).
   - blocks, concatenated in order. Each block builds the full
     `width*block_height` row-major index strip (pad the final strip's
     surplus rows with index 0), then:
     - **RLE (default):** `0x00` tag + `(count, value)` byte pairs
       run-length-encoding the strip; split any run > 255 into
       consecutive <=255 same-value pairs; the pair expansions sum to
       exactly `width*block_height`. (section 4 `eaf_decode_rle`.)
     - **RAW:** `0x05` tag + the `width*block_height` strip bytes
       verbatim. (section 4 `eaf_decode_raw`.)
4. After writing offsets 16..EOF, compute
   `stored_chk = sum of bytes[16..EOF]` (u32 wrap) and
   `stored_len = EOF - 16`; patch them into the header.

All multi-byte fields little-endian. Constraints: `width`,
`height`, `blocks`, `block_height` all > 0;
`blocks*block_height >= height`; `bit_depth == 8`; header+table+palette
size (`data_offset`) < 65536 (it is a `uint16_t`); never emit `_C`.
RLE additionally: every run > 255 split into <=255 pairs; pair
expansions sum to exactly `width*block_height`; pair stream even
length (the decoder's `in_pos+1 < input_size` guard,
`gfx_eaf_dec.c:418`, drops a lone trailing byte).

---

## 6. OPEN QUESTIONS

The host round-trip gate (`tools/eaf/test_eaf_roundtrip.py` vs the
decoder-mirroring `tools/eaf/eaf_refdec.py`) now covers BOTH RAW and
RLE pixel-exact, so the items below that depended on it are RESOLVED.

1. **`_C` frame body layout** -- RESOLVED (avoided). Undeterminable from
   this decoder; `_C` is recognised by tag and rejected by the consumer
   (`gfx_anim.c:234`). Decision stands: never emit `_C`. (section 2)
2. **`_S` offset-2 separator byte** -- RESOLVED. Never read by the
   decoder; emitting `0x00` round-trips correctly for both RAW and RLE
   across all test cases. (section 2)
3. **Palette transparency semantics** -- RESOLVED. The transparency
   model is now mirrored exactly against the *renderer*
   (`gfx_anim_render_8bit_pixels`, gfx_anim.c:470-499): a `[0,0,0,0]`
   index is a skipped (background-preserving) pixel, and the encoder
   rejects any *used* `[0,0,0,0]` index. The host round-trip
   (`transparency_over_bg`, `opaque_black_frame`,
   `encode_rejects_transparent`) confirms on-device pixel values for
   both encodings. Encoder uses A=0xFF for opaque black. (section 4)
4. **`version[6]` bytes** -- RESOLVED (non-issue). Copied but never
   validated (`gfx_eaf_dec.c:210`); zeros accepted; round-trip clean.
5. **RLE `count == 0` semantics** -- RESOLVED. A `(0, value)` pair is a
   valid decoder no-op (the `gfx_eaf_dec.c:428/435` write loops do not
   run; the `:422` bounds check is harmless). The encoder never emits
   one (every run length >= 1). (section 4)
6. **RLE max run / >255 split** -- RESOLVED. `repeat_count` is `uint8`
   (`gfx_eaf_dec.c:419`); the encoder splits runs > 255 into
   consecutive <=255 same-value pairs, verified by the
   `rle_run_over_255` / `rle_run_far_over_255` cases. (section 4)
7. **RLE pointer/size passed to the decoder** -- RESOLVED. Confirmed
   identical to RAW: `eaf_decode_block` passes `block_data + 1,
   block_len - 1` for every encoding (`gfx_eaf_dec.c:400`), so the
   `0x00` tag is stripped before `eaf_decode_rle` and
   `input_size = block_len - 1`. (section 4)

None of these block writing a byte-correct `_S` + RLE/RAW + 8-bit
encoder; the encoder deliberately avoids every ambiguous path and the
host round-trip vs the decoder-mirroring refdec is the gate.
