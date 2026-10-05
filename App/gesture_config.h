#ifndef GESTURE_CONFIG_H
#define GESTURE_CONFIG_H

#include "gesture_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_HEADER_BYTES (24u + 4u * GE_MAX_CLASSES)
#define GC_BLOB_MAX (GE_MODEL_BLOB_MAX + GC_HEADER_BYTES)
#define GC_RGB_HOLD_MIN_MS 100u
#define GC_RGB_HOLD_MAX_MS 30000u
#define GC_RGB_HOLD_STEP_MS 100u

typedef struct {
    uint8_t class_limit;
    uint8_t demo_target;
    uint16_t rgb_hold_ms;
    uint32_t colors[GE_MAX_CLASSES]; /* 0x00RRGGBB, including hidden slots. */
} gesture_config_t;

void gc_defaults(gesture_config_t *config);
ge_status_t gc_validate(const gesture_config_t *config);
/* One versioned little-endian transaction contains settings and the complete
 * existing GE model, protected by an outer CRC32. Zero means export failure. */
size_t gc_export(const gesture_config_t *config, const ge_engine_t *engine,
                 uint8_t *buffer, size_t capacity);
/* Accepts current and v1 configuration envelopes and old bare GE model blobs.
 * Missing settings receive defaults without changing class IDs/templates.
 * Every error leaves the existing engine and config unchanged. */
ge_status_t gc_import(gesture_config_t *config, ge_engine_t *engine,
                      const uint8_t *buffer, size_t length);

#ifdef __cplusplus
}
#endif
#endif
