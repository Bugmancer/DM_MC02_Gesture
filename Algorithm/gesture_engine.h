#ifndef GESTURE_ENGINE_H
#define GESTURE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GE_MAX_CLASSES 8u
#define GE_TEMPLATES_PER_CLASS 3u
#define GE_POINTS 48u
#define GE_FEATURES 6u
#define GE_NAME_BYTES 16u
#define GE_MAX_SAMPLES 512u
#define GE_STREAM_SAMPLES (GE_MAX_SAMPLES + 32u)
#define GE_PRE_ROLL_SAMPLES 16u
#define GE_MODEL_BLOB_MAX 15000u
#define GE_CLASS_NONE 255u

typedef struct {
    uint32_t timestamp_ms;
    float ax, ay, az; /* m/s^2 */
    float gx, gy, gz; /* rad/s */
} ge_sample_t;

typedef enum {
    GE_OK = 0, GE_ERR_ARGUMENT, GE_ERR_BUSY, GE_ERR_FULL,
    GE_ERR_TOO_SHORT, GE_ERR_TOO_LONG, GE_ERR_QUALITY,
    GE_ERR_INCONSISTENT, GE_ERR_CONFLICT, GE_ERR_NOT_READY,
    GE_ERR_FORMAT, GE_ERR_CRC, GE_ERR_SAMPLE_GAP
} ge_status_t;

typedef enum {
    GE_EVENT_NONE = 0, GE_EVENT_SEGMENT_FINISHED, GE_EVENT_RECOGNIZED,
    GE_EVENT_UNKNOWN, GE_EVENT_DEMO_ACCEPTED, GE_EVENT_DEMO_REJECTED,
    GE_EVENT_TRAIN_READY, GE_EVENT_MATCH, GE_EVENT_DEMO_WARNING
} ge_event_type_t;

typedef struct {
    ge_event_type_t type;
    ge_status_t status;
    uint8_t class_id;
    uint8_t training_count;
    uint32_t duration_ms;
    float distance;
    float second_distance; /* DEMO_WARNING: similarity boundary, not runner-up. */
} ge_event_t;

typedef struct {
    int16_t values[GE_POINTS][GE_FEATURES];
    uint16_t duration_ms;
    uint16_t reserved;
} ge_template_t;

typedef struct {
    uint8_t used;
    uint8_t template_count;
    uint16_t reserved;
    char name[GE_NAME_BYTES];
    float threshold;
    ge_template_t templates[GE_TEMPLATES_PER_CLASS];
} ge_class_t;

typedef struct {
    ge_class_t classes[GE_MAX_CLASSES];
} ge_model_t;

/* Fixed storage, no allocator. Treat fields below as private except model. */
typedef struct {
    ge_model_t model;
    ge_template_t pending[GE_TEMPLATES_PER_CLASS];
    int16_t samples[GE_MAX_SAMPLES][GE_FEATURES];
    int16_t pre_roll[GE_PRE_ROLL_SAMPLES][GE_FEATURES];
    float dtw_rows[2][GE_POINTS + 3u];
    int16_t stream_samples[GE_STREAM_SAMPLES][GE_FEATURES];
    float stream_best, stream_second, stream_class_best;
    uint32_t stream_cycle_ms, stream_quiet_ms, stream_start_ms;
    uint16_t stream_head, stream_count, stream_end, stream_available, stream_duration, stream_class_duration;
    uint8_t streaming, stream_cycle, stream_class, stream_scale, stream_id;
    uint8_t stream_candidate, stream_verify, stream_latched;
    uint16_t stream_active_ms, stream_strong_samples;
    float gravity[3];
    char pending_name[GE_NAME_BYTES];
    ge_status_t training_error, capture_error;
    uint32_t last_ms, settle_ms, start_ms, active_ms, quiet_ms, cooldown_ms, strong_ms;
    uint16_t sample_count, pre_count, pre_head, strong_samples;
    uint8_t initialized, recognizing, capturing, training;
    uint8_t training_count, pending_ready, start_count, pending_class_id;
    uint8_t armed, manual_training;
    float pending_threshold;
    ge_event_t events[4];
    uint8_t event_read, event_write, event_count;
} ge_engine_t;

void ge_init(ge_engine_t *engine);
/* Discard stream timing/capture/events and require fresh samples again.
 * Legacy automatic segmentation also requires stationary settling again;
 * streaming recognition starts from the next usable sample.
 * Saved classes and accepted/pending training demonstrations are preserved. */
void ge_reset_stream(ge_engine_t *engine);
/* Only controls classification; segmentation and demonstration input continue. */
void ge_set_recognition(ge_engine_t *engine, int enabled);
/* Rolling DTW previews during movement, plus at most one actionable recognition
 * until 400 ms of quiet. Matching is spread over samples (at most three DTWs per
 * feed); automatic training and the stored template format are unchanged. */
void ge_set_streaming_recognition(ge_engine_t *engine, int enabled);
/* Default is automatic. Changing this setting discards an in-flight segment;
 * manual input changes learning only, never recognition segmentation. */
void ge_set_manual_training(ge_engine_t *engine, int enabled);
void ge_feed(ge_engine_t *engine, const ge_sample_t *sample);
int ge_next_event(ge_engine_t *engine, ge_event_t *event);

/* Begin a new class: manual input needs one demonstration, with up to three
 * optional refinements; automatic input needs three. Then explicit confirmation.
 * Existing classes are never overwritten. Failed trials can be repeated.
 * Similarity to an existing class warns after accepting a manual demonstration;
 * automatic enrollment still rejects conflicts. Recognition ambiguity rejects.
 * Training suppresses recognition. Cancel also discards pending templates. */
ge_status_t ge_train_begin(ge_engine_t *engine, const char *name);
ge_status_t ge_train_begin_at(ge_engine_t *engine, uint8_t class_id, const char *name);
/* Manual learning captures exactly the samples between press and release,
 * without pre-roll or quiet-tail trimming. A press may precede the first IMU
 * sample; the first usable sample seeds gravity without stationary settling.
 * Invalid/overlong captures stay latched until release and are then rejected. */
ge_status_t ge_train_capture_begin(ge_engine_t *engine, uint32_t now_ms);
ge_status_t ge_train_capture_end(ge_engine_t *engine, uint32_t now_ms);
int ge_training_capturing(const ge_engine_t *engine);
void ge_train_cancel(ge_engine_t *engine);
ge_status_t ge_train_confirm(ge_engine_t *engine, uint8_t *class_id);
uint8_t ge_training_progress(const ge_engine_t *engine);
int ge_training_ready(const ge_engine_t *engine);
ge_status_t ge_training_error(const ge_engine_t *engine);
uint8_t ge_class_count(const ge_engine_t *engine);
const ge_class_t *ge_class_get(const ge_engine_t *engine, uint8_t class_id);
ge_status_t ge_class_delete(ge_engine_t *engine, uint8_t class_id);

/* Explicit little-endian versioned wire format with CRC32; never struct dumps.
 * Import validates everything before changing the current model. */
size_t ge_model_export(const ge_engine_t *engine, uint8_t *data, size_t capacity);
ge_status_t ge_model_import(ge_engine_t *engine, const uint8_t *data, size_t length);
const char *ge_status_string(ge_status_t status);

/* Six-axis features retain gravity-axis signs and horizontal magnitudes.
 * They tolerate initial tilt but cannot distinguish absolute heading or
 * mirrored horizontal motions. Ambiguous recognition is rejected. */

#ifdef __cplusplus
}
#endif
#endif
