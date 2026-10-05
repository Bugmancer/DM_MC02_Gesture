# Board Gesture Configuration

`gesture_config.h` stores user settings with the complete gesture model in one
buffer. The application passes that buffer to one `board_store_save()` call, so
settings and templates share the board store's existing transaction boundary.

## Settings

| Field | Range | Default |
| --- | --- | --- |
| `class_limit` | 1 through 8 | 8 |
| `demo_target` | 1 through 3 | 1 |
| `rgb_hold_ms` | 100 through 30000 in 100 ms steps | 3000 |
| `colors[8]` | `0x000000` through `0xFFFFFF` | Existing eight board colors |

Colors are packed `0x00RRGGBB`; each RGB channel spans 0 through 255. Defaults
retain the previous board brightness: red, green, blue, yellow, cyan, magenta,
orange and white use the existing `0x18` maximum channel values. Black is valid.

The class limit enables slots `[0, class_limit)`. Reducing it does not delete,
renumber or rewrite learned classes. Hidden classes remain in every export and
become available when the limit increases. Recognition and similarity warnings
ignore hidden classes; new learning cannot target a hidden slot. Existing
classes are never overwritten by enrollment.

`demo_target` is the application's confirmation requirement. It does not change
the engine's storage capacity of three templates per class or rewrite existing
models. The application enforces the configured target before confirmation.

## API

Firmware r9 accepts a complete USB configuration command:

```text
configure 2 3 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 5000
```

The arguments are the class limit, demonstration target and exactly eight
six-digit RGB colors and the RGB hold time in milliseconds. Omitting the hold
time preserves the current value, so r8 GUI commands remain valid. The longest
command is 76 bytes including LF and fits the 80-byte USB input buffer. Changes
are rejected during learning, deletion confirmation or on-board settings.
One flash transaction saves the complete settings and model; success emits
`CONFIG` / `COLOR` / `TIMING` records followed by `CONFIG_RESULT,SAVED`. Failure emits
`CONFIG_RESULT,FAILED` and restores the previous settings. Recognition resumes
when the sensor and sampling timer remain ready. The GUI waits for this result
and does not apply its local draft optimistically.

```c
gesture_config_t config;
ge_engine_t engine;
uint8_t blob[GC_BLOB_MAX];
size_t length;

ge_init(&engine);
gc_defaults(&config);
/* On startup, gc_import also accepts the previous bare GE model format. */
/* gc_import(&config, &engine, stored_data, stored_length); */

length = gc_export(&config, &engine, blob, sizeof(blob));
/* Commit the entire nonzero-length buffer with board_store_save(). */
```

- `gc_defaults()` initializes all fields and padding.
- `gc_validate()` checks settings and returns `GE_ERR_ARGUMENT` for bad ranges.
- `gc_export()` writes an explicit little-endian envelope and model, returning
  its byte length or zero. It does not change settings or engine state.
- `gc_import()` validates the envelope, settings and embedded model before
  changing either settings or engine state. All errors leave both unchanged.
  Active engine training/capture returns `GE_ERR_BUSY`.
- `ge_set_class_limit()` applies a validated runtime limit without modifying
  saved model contents. A limit change during training returns `GE_ERR_BUSY`.
  Successful changes clear in-flight classification and queued match events.
- `ge_active_class_count()` counts only enabled, learned classes.
  `ge_class_count()` and `ge_class_get()` continue to expose all stored classes.

## Storage Format

The `GCF1` version 2 envelope is `GC_HEADER_BYTES` (currently 56) bytes, followed
by the unmodified `GDT1` model format. It contains the version, header and total
lengths, settings, embedded model length, and an outer
CRC32. The outer CRC includes all bytes with its own four-byte field treated as
zero. The embedded model retains its own existing CRC and validation rules.
Current exports are 14,184 bytes; `GC_BLOB_MAX` reserves 15,056 bytes.

Version 2 stores `rgb_hold_ms` in the former reserved bytes 18 and 19, in
little-endian order. Version 1 imports with a 3000 ms hold while preserving its
limits, colors and templates. The total header and model sizes do not grow.

Old bare `GDT1` models import with default settings. Class IDs, names and template
contents are preserved. The next successful save writes the new envelope.
Older firmware is not guaranteed to read this new envelope; backward import by
the new firmware is supported, firmware downgrade compatibility is not implied.

The caller still owns flash error handling and application state. In particular,
an unsuccessful flash save must not discard pending demonstrations or claim the
new configuration is persisted. The import/export layer performs no flash I/O.
