#include "gesture_engine.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define GE_GRAVITY 9.80665f
#define GE_SCALE 1024.0f
#define GE_DTW_BAND 8u
#define GE_SETTLE_MS 350u
#define GE_START_SAMPLES 6u
#define GE_END_QUIET_MS 150u
#define GE_MIN_ACTIVE_MS 120u
#define GE_MIN_STRONG_MS 30u
#define GE_MIN_STRONG_SAMPLES 6u
#define GE_STRONG_ACCEL 2.0f
#define GE_STRONG_GYRO 1.2f
#define GE_MIN_DURATION_MS 180u
#define GE_MAX_DURATION_MS 2400u
#define GE_COOLDOWN_MS 400u
#define GE_MAX_GAP_MS 30u
#define GE_MIN_THRESHOLD 0.055f
#define GE_MAX_THRESHOLD 0.180f
#define GE_DEMO_MAX_DISTANCE 0.240f
#define GE_MANUAL_DEMO_MAX_DISTANCE 0.300f
#define GE_SINGLE_THRESHOLD 0.100f
#define GE_STREAM_INTERVAL_MS 20u
#define GE_STREAM_RESERVE 64u
#define GE_WIRE_HEADER 16u
#define GE_WIRE_TEMPLATE (4u + GE_POINTS * GE_FEATURES * 2u)
#define GE_WIRE_CLASS (24u + GE_TEMPLATES_PER_CLASS * GE_WIRE_TEMPLATE)
#define GE_WIRE_SIZE (GE_WIRE_HEADER + GE_MAX_CLASSES * GE_WIRE_CLASS)

static float ge_min(float a, float b) { return a < b ? a : b; }
static float ge_max(float a, float b) { return a > b ? a : b; }

static void reset_stream_match(ge_engine_t *e)
{
    e->stream_head = e->stream_count = e->stream_available = 0u;
    e->stream_cycle = e->stream_verify = e->stream_latched = 0u;
    e->stream_candidate = e->stream_id = GE_CLASS_NONE;
    e->stream_quiet_ms = 0u;
    e->stream_cycle_ms = e->last_ms;
    e->stream_active_ms = 0u;
    e->stream_strong_samples = 0u;
}

static float norm3(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

static int16_t quantize(float value)
{
    float scaled = value * GE_SCALE;
    if (scaled > 16384.0f) scaled = 16384.0f;
    if (scaled < -16384.0f) scaled = -16384.0f;
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void emit(ge_engine_t *e, ge_event_type_t type, ge_status_t status,
                 uint8_t id, uint32_t duration, float distance, float second)
{
    ge_event_t *event;
    if (e->event_count == 4u) {
        e->event_read = (uint8_t)((e->event_read + 1u) % 4u);
        --e->event_count;
    }
    event = &e->events[e->event_write];
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->status = status;
    event->class_id = id;
    event->duration_ms = duration;
    event->training_count = e->training_count;
    event->distance = distance;
    event->second_distance = second;
    e->event_write = (uint8_t)((e->event_write + 1u) % 4u);
    ++e->event_count;
}

static void reset_capture(ge_engine_t *e, uint32_t now)
{
    e->capturing = 0u;
    e->capture_error = GE_OK;
    e->sample_count = 0u;
    e->start_count = 0u;
    e->pre_count = 0u;
    e->pre_head = 0u;
    e->active_ms = 0u;
    e->strong_ms = 0u;
    e->strong_samples = 0u;
    e->quiet_ms = 0u;
    e->cooldown_ms = now;
    e->armed = 0u;
}

void ge_init(ge_engine_t *e)
{
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->recognizing = 1u;
    e->class_limit = GE_MAX_CLASSES;
    reset_stream_match(e);
}

void ge_set_recognition(ge_engine_t *e, int enabled)
{
    if (e) {
        e->recognizing = enabled ? 1u : 0u;
        reset_stream_match(e);
    }
}

void ge_set_streaming_recognition(ge_engine_t *e, int enabled)
{
    if (!e) return;
    e->streaming = enabled ? 1u : 0u;
    reset_capture(e, e->last_ms);
    reset_stream_match(e);
}

void ge_set_manual_training(ge_engine_t *e, int enabled)
{
    uint8_t manual = enabled ? 1u : 0u;
    if (!e || e->manual_training == manual) return;
    reset_capture(e, e->last_ms);
    e->manual_training = manual;
}

ge_status_t ge_set_class_limit(ge_engine_t *e, uint8_t limit)
{
    if (!e || limit == 0u || limit > GE_MAX_CLASSES) return GE_ERR_ARGUMENT;
    if (e->class_limit == limit) return GE_OK;
    if (e->training) return GE_ERR_BUSY;
    e->class_limit = limit;
    reset_capture(e, e->last_ms);
    reset_stream_match(e);
    e->event_count = e->event_read = e->event_write = 0u;
    return GE_OK;
}

void ge_reset_stream(ge_engine_t *e)
{
    if (!e) return;
    reset_capture(e, e->last_ms);
    e->initialized = 0u;
    reset_stream_match(e);
    e->event_count = e->event_read = e->event_write = 0u;
}

int ge_next_event(ge_engine_t *e, ge_event_t *event)
{
    if (!e || !event || !e->event_count) return 0;
    *event = e->events[e->event_read];
    e->event_read = (uint8_t)((e->event_read + 1u) % 4u);
    --e->event_count;
    return 1;
}

uint8_t ge_class_count(const ge_engine_t *e)
{
    uint8_t i, count = 0u;
    if (!e) return 0u;
    for (i = 0u; i < GE_MAX_CLASSES; ++i) count += e->model.classes[i].used ? 1u : 0u;
    return count;
}

uint8_t ge_active_class_count(const ge_engine_t *e)
{
    uint8_t i, count = 0u;
    if (!e) return 0u;
    for (i = 0u; i < e->class_limit; ++i) count += e->model.classes[i].used ? 1u : 0u;
    return count;
}

const ge_class_t *ge_class_get(const ge_engine_t *e, uint8_t id)
{
    if (!e || id >= GE_MAX_CLASSES || !e->model.classes[id].used) return NULL;
    return &e->model.classes[id];
}

uint8_t ge_training_progress(const ge_engine_t *e) { return e ? e->training_count : 0u; }
int ge_training_ready(const ge_engine_t *e) { return e && e->pending_ready; }
int ge_training_capturing(const ge_engine_t *e)
{
    return e && e->training && e->manual_training && e->capturing;
}
ge_status_t ge_training_error(const ge_engine_t *e)
{
    return e ? e->training_error : GE_ERR_ARGUMENT;
}

ge_status_t ge_train_begin(ge_engine_t *e, const char *name)
{
    uint8_t id;
    if (!e) return GE_ERR_ARGUMENT;
    for (id = 0u; id < e->class_limit && e->model.classes[id].used; ++id) {}
    if (id == e->class_limit) return GE_ERR_FULL;
    return ge_train_begin_at(e, id, name);
}

ge_status_t ge_train_begin_at(ge_engine_t *e, uint8_t id, const char *name)
{
    size_t n;
    if (!e || !name || id >= e->class_limit) return GE_ERR_ARGUMENT;
    if (e->training || e->capturing) return GE_ERR_BUSY;
    if (ge_active_class_count(e) >= e->class_limit) return GE_ERR_FULL;
    if (e->model.classes[id].used) return GE_ERR_CONFLICT;
    for (n = 0u; n < GE_NAME_BYTES && name[n]; ++n) {
        if ((unsigned char)name[n] < 32u || (unsigned char)name[n] > 126u) return GE_ERR_ARGUMENT;
    }
    if (n == 0u || n == GE_NAME_BYTES) return GE_ERR_ARGUMENT;
    memset(e->pending, 0, sizeof(e->pending));
    memset(e->pending_name, 0, sizeof(e->pending_name));
    memcpy(e->pending_name, name, n);
    e->training = 1u;
    e->pending_class_id = id;
    e->training_count = 0u;
    e->pending_ready = 0u;
    e->training_error = GE_OK;
    e->pending_threshold = 0.0f;
    e->event_count = e->event_read = e->event_write = 0u;
    reset_capture(e, e->last_ms);
    reset_stream_match(e);
    return GE_OK;
}

ge_status_t ge_train_capture_begin(ge_engine_t *e, uint32_t now)
{
    if (!e) return GE_ERR_ARGUMENT;
    if (!e->manual_training || !e->training || e->training_count >= GE_TEMPLATES_PER_CLASS) return GE_ERR_NOT_READY;
    if (e->capturing) return GE_ERR_BUSY;
    if (now - e->last_ms > GE_MAX_GAP_MS) e->initialized = 0u;
    reset_capture(e, now);
    e->capturing = 1u;
    e->start_ms = now;
    e->training_error = GE_OK;
    return GE_OK;
}

void ge_train_cancel(ge_engine_t *e)
{
    if (!e) return;
    e->training = e->training_count = e->pending_ready = 0u;
    e->training_error = GE_OK;
    e->pending_threshold = 0.0f;
    memset(e->pending, 0, sizeof(e->pending));
    memset(e->pending_name, 0, sizeof(e->pending_name));
    e->event_count = e->event_read = e->event_write = 0u;
    reset_capture(e, e->last_ms);
    reset_stream_match(e);
}

ge_status_t ge_train_confirm(ge_engine_t *e, uint8_t *class_id)
{
    uint8_t id;
    ge_class_t *c;
    if (!e) return GE_ERR_ARGUMENT;
    if (!e->training || !e->pending_ready || e->capturing) return GE_ERR_NOT_READY;
    id = e->pending_class_id;
    if (id >= e->class_limit || e->model.classes[id].used) return GE_ERR_CONFLICT;
    c = &e->model.classes[id];
    memset(c, 0, sizeof(*c));
    c->used = 1u;
    c->template_count = e->training_count;
    c->threshold = e->pending_threshold;
    memcpy(c->name, e->pending_name, sizeof(c->name));
    memcpy(c->templates, e->pending, sizeof(c->templates));
    ge_train_cancel(e);
    if (class_id) *class_id = id;
    return GE_OK;
}

ge_status_t ge_class_delete(ge_engine_t *e, uint8_t id)
{
    if (!e || id >= GE_MAX_CLASSES || !e->model.classes[id].used) return GE_ERR_ARGUMENT;
    if (e->training || e->capturing) return GE_ERR_BUSY;
    memset(&e->model.classes[id], 0, sizeof(e->model.classes[id]));
    reset_stream_match(e);
    return GE_OK;
}

/* Normalize time globally, then allow local timing differences inside the band.
 * Cost uses two rows, so no per-template alignment matrix is retained. */
static float distance_between(ge_engine_t *e, const ge_template_t *a, const ge_template_t *b)
{
    uint16_t i, j, k;
    const uint16_t count = GE_POINTS + 2u;
    float *previous = e->dtw_rows[0];
    float *current = e->dtw_rows[1];
    float *swap;
    /* Implicit stationary endpoints let DTW align brief key-edge pauses with
     * automatically segmented queries. Every recorded point still participates
     * and the original cost normalization and acceptance thresholds are kept. */
    for (j = 0u; j <= count; ++j) previous[j] = FLT_MAX;
    previous[0] = 0.0f;
    for (i = 1u; i <= count; ++i) {
        uint16_t begin = i > GE_DTW_BAND ? (uint16_t)(i - GE_DTW_BAND) : 1u;
        uint16_t end = i + GE_DTW_BAND < count ? (uint16_t)(i + GE_DTW_BAND) : count;
        for (j = 0u; j <= count; ++j) current[j] = FLT_MAX;
        for (j = begin; j <= end; ++j) {
            float cost = 0.0f;
            for (k = 0u; k < GE_FEATURES; ++k) {
                float av = (i == 1u || i == count) ? 0.0f : (float)a->values[i - 2u][k];
                float bv = (j == 1u || j == count) ? 0.0f : (float)b->values[j - 2u][k];
                float difference = (av - bv) / GE_SCALE;
                cost += difference * difference;
            }
            current[j] = cost / (float)GE_FEATURES +
                         ge_min(previous[j - 1u], ge_min(previous[j], current[j - 1u]));
        }
        swap = previous;
        previous = current;
        current = swap;
    }
    return sqrtf(previous[count] / (float)GE_POINTS);
}

static float class_distance(ge_engine_t *e, const ge_template_t *sample, const ge_class_t *c)
{
    uint8_t i;
    float best = FLT_MAX, second = FLT_MAX;
    for (i = 0u; i < c->template_count; ++i) {
        float d = distance_between(e, sample, &c->templates[i]);
        if (d < best) { second = best; best = d; }
        else if (d < second) second = d;
    }
    /* Require support from two demonstrations; a single outlier cannot win. */
    return second == FLT_MAX ? best : 0.5f * (best + second);
}

static void reject_segment(ge_engine_t *e, ge_status_t status, uint32_t duration)
{
    emit(e, GE_EVENT_SEGMENT_FINISHED, status, GE_CLASS_NONE, duration, 0.0f, 0.0f);
    if (e->training && e->training_count < GE_TEMPLATES_PER_CLASS) {
        e->training_error = status;
        emit(e, GE_EVENT_DEMO_REJECTED, status, GE_CLASS_NONE, duration, 0.0f, 0.0f);
    } else if (e->recognizing && !e->training) {
        emit(e, GE_EVENT_UNKNOWN, status, GE_CLASS_NONE, duration, 0.0f, 0.0f);
    }
}

static void learn_segment(ge_engine_t *e, const ge_template_t *sample)
{
    uint8_t i, j, conflict_id = GE_CLASS_NONE;
    float worst = 0.0f, threshold;
    float conflict_distance = FLT_MAX, conflict_limit = 0.0f;
    float demo_limit = e->manual_training ? GE_MANUAL_DEMO_MAX_DISTANCE : GE_DEMO_MAX_DISTANCE;
    ge_status_t status = GE_OK;
    for (i = 0u; i < e->training_count; ++i) {
        float d = distance_between(e, sample, &e->pending[i]);
        worst = ge_max(worst, d);
        if (d > demo_limit) status = GE_ERR_INCONSISTENT;
        for (j = 0u; j < i; ++j)
            worst = ge_max(worst, distance_between(e, &e->pending[i], &e->pending[j]));
    }
    threshold = ge_max(e->manual_training ? GE_SINGLE_THRESHOLD : GE_MIN_THRESHOLD,
                       1.6f * worst + 0.025f);
    if (threshold > GE_MAX_THRESHOLD) {
        /* Manual enrollment tolerates variation without widening recognition.
         * At a 0.30 pair limit, replaying a stored demo has two-template cost
         * at most 0.15, below the unchanged 0.18 recognition ceiling. */
        if (e->manual_training) threshold = GE_MAX_THRESHOLD;
        else status = GE_ERR_INCONSISTENT;
    }
    for (i = 0u; i < e->class_limit && status == GE_OK; ++i) {
        const ge_class_t *c = &e->model.classes[i];
        if (!c->used) continue;
        /* This broad boundary is advisory for explicit manual enrollment.
         * Recognition still requires an unambiguous winning class. */
        for (j = 0u; j < c->template_count; ++j) {
            float limit = 1.5f * ge_max(c->threshold, threshold) + 0.02f;
            float distance = distance_between(e, sample, &c->templates[j]);
            uint8_t pending;
            for (pending = 0u; pending < e->training_count; ++pending)
                distance = ge_min(distance, distance_between(e, &e->pending[pending], &c->templates[j]));
            if (distance < limit && distance < conflict_distance) {
                conflict_distance = distance;
                conflict_limit = limit;
                conflict_id = i;
            }
        }
    }
    if (conflict_id != GE_CLASS_NONE && !e->manual_training) status = GE_ERR_CONFLICT;
    e->training_error = status;
    if (status != GE_OK) {
        emit(e, GE_EVENT_DEMO_REJECTED, status, GE_CLASS_NONE, sample->duration_ms, worst, 0.0f);
        return;
    }
    e->pending[e->training_count++] = *sample;
    e->pending_threshold = threshold;
    emit(e, GE_EVENT_DEMO_ACCEPTED, GE_OK, GE_CLASS_NONE, sample->duration_ms, worst, 0.0f);
    if (conflict_id != GE_CLASS_NONE)
        emit(e, GE_EVENT_DEMO_WARNING, GE_ERR_CONFLICT, conflict_id,
             sample->duration_ms, conflict_distance, conflict_limit);
    if (!e->pending_ready && (e->manual_training || e->training_count == GE_TEMPLATES_PER_CLASS)) {
        e->pending_ready = 1u;
        emit(e, GE_EVENT_TRAIN_READY, GE_OK, GE_CLASS_NONE, sample->duration_ms, worst, 0.0f);
    }
}

static void recognize_segment(ge_engine_t *e, const ge_template_t *sample)
{
    uint8_t i, id = GE_CLASS_NONE;
    float best = FLT_MAX, second = FLT_MAX;
    for (i = 0u; i < e->class_limit; ++i) {
        float d;
        if (!e->model.classes[i].used) continue;
        d = class_distance(e, sample, &e->model.classes[i]);
        if (d < best) { second = best; best = d; id = i; }
        else if (d < second) second = d;
    }
    if (id != GE_CLASS_NONE && best <= e->model.classes[id].threshold &&
        (second == FLT_MAX || (second - best >= 0.025f && best <= second * 0.78f))) {
        emit(e, GE_EVENT_RECOGNIZED, GE_OK, id, sample->duration_ms, best, second);
    } else {
        emit(e, GE_EVENT_UNKNOWN, GE_OK, GE_CLASS_NONE, sample->duration_ms, best, second);
    }
}

static int feature_active(const int16_t *feature)
{
    return feature[0] > 70 || feature[3] > 125;
}

static void normalize_template(ge_template_t *sample)
{
    uint16_t i, k;
    float acc_peak = 0.08f * GE_SCALE, gyro_peak = 0.18f * GE_SCALE;
    for (i = 0u; i < GE_POINTS; ++i) {
        acc_peak = ge_max(acc_peak, (float)sample->values[i][0]);
        gyro_peak = ge_max(gyro_peak, (float)sample->values[i][3]);
    }
    for (i = 0u; i < GE_POINTS; ++i) {
        for (k = 0u; k < GE_FEATURES; ++k) {
            float divisor = k < 3u ? acc_peak : gyro_peak;
            sample->values[i][k] = (int16_t)((float)sample->values[i][k] * GE_SCALE / divisor);
        }
    }
}

static int stream_template(ge_engine_t *e, ge_template_t *sample, uint16_t count)
{
    uint16_t i, k, strong = 0u, active = 0u;
    uint16_t first = (uint16_t)((e->stream_end + GE_STREAM_SAMPLES - count) % GE_STREAM_SAMPLES);
    if (count < 36u || count > e->stream_available) return 0;
    for (i = 0u; i < count; ++i) {
        const int16_t *feature = e->stream_samples[(first + i) % GE_STREAM_SAMPLES];
        if (feature[0] >= 209 || feature[3] >= 410) ++strong;
        if (feature_active(feature)) ++active;
    }
    if (strong < GE_MIN_STRONG_SAMPLES || active < 24u) return 0;
    memset(sample, 0, sizeof(*sample));
    sample->duration_ms = (uint16_t)(count * 5u);
    for (i = 0u; i < GE_POINTS; ++i) {
        float position = (float)i * (float)(count - 1u) / (float)(GE_POINTS - 1u);
        uint16_t base = (uint16_t)position;
        uint16_t next = base + 1u < count ? (uint16_t)(base + 1u) : base;
        float fraction = position - base;
        for (k = 0u; k < GE_FEATURES; ++k) {
            float a = e->stream_samples[(first + base) % GE_STREAM_SAMPLES][k];
            float b = e->stream_samples[(first + next) % GE_STREAM_SAMPLES][k];
            sample->values[i][k] = (int16_t)(a + fraction * (b - a));
        }
    }
    normalize_template(sample);
    return 1;
}

/* One class/scale per feed bounds foreground work to three existing DTWs.
 * The cycle freezes its window endpoint; 64 reserved ring slots protect that
 * history during two 8 * 3 scans. No motion-end decision is needed. */
static void stream_match(ge_engine_t *e, const int16_t *feature, uint32_t elapsed, int low)
{
    static const float scales[] = { 0.65f, 1.0f, 1.5f };
    ge_template_t sample;
    const ge_class_t *c;
    uint16_t count;
    uint8_t i, id;
    uint32_t duration = 0u;
    if (!e->recognizing || !ge_active_class_count(e)) return;
    memcpy(e->stream_samples[e->stream_head], feature, sizeof(e->stream_samples[0]));
    e->stream_head = (uint16_t)((e->stream_head + 1u) % GE_STREAM_SAMPLES);
    if (e->stream_count < GE_STREAM_SAMPLES - GE_STREAM_RESERVE) ++e->stream_count;
    if (low) {
        if (!e->stream_active_ms) e->stream_start_ms = e->last_ms;
        if (e->stream_active_ms < GE_MIN_ACTIVE_MS) e->stream_active_ms += (uint16_t)elapsed;
        if ((feature[0] >= 209 || feature[3] >= 410) && e->stream_strong_samples < GE_MIN_STRONG_SAMPLES)
            ++e->stream_strong_samples;
        e->stream_quiet_ms = 0u;
    }
    else if (e->stream_quiet_ms < GE_COOLDOWN_MS) e->stream_quiet_ms += elapsed;
    if (e->stream_quiet_ms >= GE_COOLDOWN_MS) {
        if (!e->stream_latched && e->stream_active_ms >= GE_MIN_ACTIVE_MS)
            emit(e, GE_EVENT_UNKNOWN, e->stream_strong_samples >= GE_MIN_STRONG_SAMPLES ? GE_OK : GE_ERR_QUALITY,
                 GE_CLASS_NONE, e->last_ms - e->stream_start_ms - e->stream_quiet_ms, FLT_MAX, FLT_MAX);
        if (e->stream_latched || e->stream_candidate != GE_CLASS_NONE)
            emit(e, GE_EVENT_MATCH, GE_OK, GE_CLASS_NONE, 0u, FLT_MAX, FLT_MAX);
        e->stream_latched = e->stream_verify = e->stream_cycle = 0u;
        e->stream_candidate = GE_CLASS_NONE;
        e->stream_count = 0u;
        e->stream_active_ms = 0u;
        e->stream_strong_samples = 0u;
        return;
    }
    if (!e->stream_cycle) {
        if (e->last_ms - e->stream_cycle_ms < GE_STREAM_INTERVAL_MS) return;
        e->stream_cycle_ms = e->last_ms;
        e->stream_end = e->stream_head;
        e->stream_available = e->stream_count;
        e->stream_class = e->stream_scale = 0u;
        e->stream_best = e->stream_second = e->stream_class_best = FLT_MAX;
        e->stream_id = GE_CLASS_NONE;
        e->stream_cycle = 1u;
    }
    while (e->stream_class < e->class_limit && !e->model.classes[e->stream_class].used)
        ++e->stream_class;
    if (e->stream_class < e->class_limit) {
        c = &e->model.classes[e->stream_class];
        for (i = 0u; i < c->template_count; ++i) duration += c->templates[i].duration_ms;
        duration /= c->template_count;
        count = (uint16_t)((float)duration * scales[e->stream_scale] / 5.0f + 0.5f);
        if (count > GE_STREAM_SAMPLES - GE_STREAM_RESERVE) count = GE_STREAM_SAMPLES - GE_STREAM_RESERVE;
        if (stream_template(e, &sample, count)) {
            float d = class_distance(e, &sample, c);
            if (d < e->stream_class_best) {
                e->stream_class_best = d;
                e->stream_class_duration = sample.duration_ms;
            }
        }
        if (++e->stream_scale < 3u) return;
        if (e->stream_class_best < e->stream_best) {
            e->stream_second = e->stream_best;
            e->stream_best = e->stream_class_best;
            e->stream_id = e->stream_class;
            e->stream_duration = e->stream_class_duration;
        } else if (e->stream_class_best < e->stream_second) e->stream_second = e->stream_class_best;
        e->stream_class_best = FLT_MAX;
        e->stream_scale = 0u;
        ++e->stream_class;
        while (e->stream_class < e->class_limit && !e->model.classes[e->stream_class].used)
            ++e->stream_class;
        if (e->stream_class < e->class_limit) return;
    }
    e->stream_cycle = 0u;
    id = e->stream_id;
    if (id == GE_CLASS_NONE || e->stream_best > e->model.classes[id].threshold ||
        (e->stream_second != FLT_MAX &&
         (e->stream_second - e->stream_best < 0.025f || e->stream_best > e->stream_second * 0.78f)))
        id = GE_CLASS_NONE;
    emit(e, GE_EVENT_MATCH, GE_OK, id, e->stream_duration, e->stream_best, e->stream_second);
    if (e->stream_verify) {
        e->stream_verify = 0u;
        if (id != GE_CLASS_NONE && id == e->stream_candidate && !e->stream_latched) {
            e->stream_latched = 1u;
            emit(e, GE_EVENT_RECOGNIZED, GE_OK, id, e->stream_duration, e->stream_best, e->stream_second);
        }
        e->stream_candidate = id;
        return;
    }
    e->stream_candidate = id;
    /* Rescan every competitor at a nearby frozen endpoint. Waiting for a new
     * moving window can skip a fast gesture at eight-class capacity; freezing
     * both endpoints preserves that evidence without using stale runner-ups. */
    if (id != GE_CLASS_NONE && !e->stream_latched) {
        e->stream_verify = e->stream_cycle = 1u;
        e->stream_end = (uint16_t)((e->stream_end + 1u) % GE_STREAM_SAMPLES);
        if (e->stream_available < GE_STREAM_SAMPLES - GE_STREAM_RESERVE) ++e->stream_available;
        e->stream_class = e->stream_scale = 0u;
        e->stream_best = e->stream_second = e->stream_class_best = FLT_MAX;
        e->stream_id = GE_CLASS_NONE;
    }
}

static void finish_segment(ge_engine_t *e, uint32_t duration, int trim)
{
    ge_template_t sample;
    uint16_t first = 0u, last, i, k;
    if (e->active_ms < GE_MIN_ACTIVE_MS || duration < GE_MIN_DURATION_MS || e->sample_count < 24u) {
        reject_segment(e, GE_ERR_TOO_SHORT, duration);
        return;
    }
    /* Check physical intensity before envelope normalization can amplify a
     * small movement into a full-size template. Require time and sample support. */
    if (e->strong_ms < GE_MIN_STRONG_MS || e->strong_samples < GE_MIN_STRONG_SAMPLES) {
        reject_segment(e, GE_ERR_QUALITY, duration);
        return;
    }
    last = (uint16_t)(e->sample_count - 1u);
    if (trim) {
        while (first < last && !feature_active(e->samples[first])) ++first;
        while (last > first && !feature_active(e->samples[last])) --last;
    }
    if ((uint16_t)(last - first) < 20u) {
        reject_segment(e, GE_ERR_QUALITY, duration);
        return;
    }
    memset(&sample, 0, sizeof(sample));
    sample.duration_ms = trim ? (uint16_t)((last - first + 1u) * 5u) : (uint16_t)duration;
    for (i = 0u; i < GE_POINTS; ++i) {
        float position = first + (float)i * (float)(last - first) / (float)(GE_POINTS - 1u);
        uint16_t base = (uint16_t)position;
        uint16_t next = base < last ? (uint16_t)(base + 1u) : base;
        float fraction = position - base;
        for (k = 0u; k < GE_FEATURES; ++k) {
            float value = e->samples[base][k] + fraction *
                          ((float)e->samples[next][k] - e->samples[base][k]);
            sample.values[i][k] = (int16_t)value;
        }
    }
    /* A faster traversal also raises acceleration/angular velocity. Normalize
     * each sensor group's envelope; keep a floor so near-zero noise stays small. */
    normalize_template(&sample);
    emit(e, GE_EVENT_SEGMENT_FINISHED, GE_OK, GE_CLASS_NONE, sample.duration_ms, 0.0f, 0.0f);
    if (e->training && e->training_count < GE_TEMPLATES_PER_CLASS) learn_segment(e, &sample);
    else if (!e->training && e->recognizing) recognize_segment(e, &sample);
}

ge_status_t ge_train_capture_end(ge_engine_t *e, uint32_t now)
{
    ge_status_t status;
    uint32_t duration;
    if (!e) return GE_ERR_ARGUMENT;
    if (!ge_training_capturing(e)) return GE_ERR_NOT_READY;
    duration = now - e->start_ms;
    status = e->capture_error;
    if (status == GE_OK && duration > GE_MAX_DURATION_MS) status = GE_ERR_TOO_LONG;
    if (status == GE_OK && now - e->last_ms > GE_MAX_GAP_MS) status = GE_ERR_SAMPLE_GAP;
    if (status != GE_OK) reject_segment(e, status, duration);
    else finish_segment(e, duration, 0);
    status = e->training_error;
    reset_capture(e, now);
    return status;
}

static void invalidate_manual_capture(ge_engine_t *e, ge_status_t status)
{
    if (e->capture_error == GE_OK) {
        e->capture_error = status;
        e->training_error = status;
    }
}

static int valid_sample(const ge_sample_t *s)
{
    return isfinite(s->ax) && isfinite(s->ay) && isfinite(s->az) &&
           isfinite(s->gx) && isfinite(s->gy) && isfinite(s->gz) &&
           fabsf(s->ax) < 200.0f && fabsf(s->ay) < 200.0f && fabsf(s->az) < 200.0f &&
           fabsf(s->gx) < 40.0f && fabsf(s->gy) < 40.0f && fabsf(s->gz) < 40.0f;
}

void ge_feed(ge_engine_t *e, const ge_sample_t *s)
{
    float acc_norm, gyro_norm, dt, gravity_norm, lx, ly, lz, linear_norm;
    float vertical_acc, vertical_gyro, nx, ny, nz;
    int16_t feature[GE_FEATURES];
    uint32_t elapsed;
    uint16_t i;
    int high, low, strong;
    if (!e || !s) return;
    if (ge_training_capturing(e) && e->capture_error != GE_OK) return;
    if (!valid_sample(s)) {
        if (ge_training_capturing(e)) invalidate_manual_capture(e, GE_ERR_QUALITY);
        else {
            if (e->capturing) reject_segment(e, GE_ERR_QUALITY, s->timestamp_ms - e->start_ms);
            reset_capture(e, s->timestamp_ms);
        }
        e->initialized = 0u;
        reset_stream_match(e);
        return;
    }
    acc_norm = norm3(s->ax, s->ay, s->az);
    gyro_norm = norm3(s->gx, s->gy, s->gz);
    if (!e->initialized) {
        if ((e->training && e->manual_training) || e->streaming) {
            /* A moving hand can seed the direction without stationary settling.
             * Zero acceleration cannot provide a gravity reference. */
            if (acc_norm <= 0.0f) return;
        } else if (acc_norm < 8.0f || acc_norm > 11.5f) return;
        e->gravity[0] = s->ax * GE_GRAVITY / acc_norm;
        e->gravity[1] = s->ay * GE_GRAVITY / acc_norm;
        e->gravity[2] = s->az * GE_GRAVITY / acc_norm;
        e->last_ms = e->settle_ms = e->cooldown_ms = s->timestamp_ms;
        e->initialized = e->streaming ? 2u : 1u;
        if (!ge_training_capturing(e) && !e->streaming) return;
        e->last_ms = s->timestamp_ms - 5u;
    }
    elapsed = s->timestamp_ms - e->last_ms;
    if (!elapsed) return;
    if (elapsed > GE_MAX_GAP_MS) {
        if (ge_training_capturing(e)) invalidate_manual_capture(e, GE_ERR_SAMPLE_GAP);
        else {
            if (e->capturing) reject_segment(e, GE_ERR_SAMPLE_GAP, e->last_ms - e->start_ms);
            reset_capture(e, s->timestamp_ms);
        }
        e->initialized = 0u;
        reset_stream_match(e);
        return;
    }
    e->last_ms = s->timestamp_ms;
    dt = (float)elapsed * 0.001f;
    nx = e->gravity[0] + (e->gravity[1] * s->gz - e->gravity[2] * s->gy) * dt;
    ny = e->gravity[1] + (e->gravity[2] * s->gx - e->gravity[0] * s->gz) * dt;
    nz = e->gravity[2] + (e->gravity[0] * s->gy - e->gravity[1] * s->gx) * dt;
    gravity_norm = norm3(nx, ny, nz);
    e->gravity[0] = nx * GE_GRAVITY / gravity_norm;
    e->gravity[1] = ny * GE_GRAVITY / gravity_norm;
    e->gravity[2] = nz * GE_GRAVITY / gravity_norm;
    if (!e->capturing && gyro_norm < 0.25f && fabsf(acc_norm - GE_GRAVITY) < 0.65f) {
        float alpha = e->initialized == 1u ? 0.08f : 0.008f;
        e->gravity[0] += alpha * (s->ax - e->gravity[0]);
        e->gravity[1] += alpha * (s->ay - e->gravity[1]);
        e->gravity[2] += alpha * (s->az - e->gravity[2]);
    }
    if (e->initialized == 1u) {
        if (ge_training_capturing(e)) e->settle_ms = s->timestamp_ms;
        else {
            if (gyro_norm > 0.25f || fabsf(acc_norm - GE_GRAVITY) > 0.65f) e->settle_ms = s->timestamp_ms;
            if (s->timestamp_ms - e->settle_ms >= GE_SETTLE_MS) e->initialized = 2u;
            return;
        }
    }
    gravity_norm = norm3(e->gravity[0], e->gravity[1], e->gravity[2]);
    nx = e->gravity[0] / gravity_norm;
    ny = e->gravity[1] / gravity_norm;
    nz = e->gravity[2] / gravity_norm;
    lx = s->ax - e->gravity[0]; ly = s->ay - e->gravity[1]; lz = s->az - e->gravity[2];
    linear_norm = norm3(lx, ly, lz);
    vertical_acc = lx * nx + ly * ny + lz * nz;
    vertical_gyro = s->gx * nx + s->gy * ny + s->gz * nz;
    feature[0] = quantize(linear_norm / GE_GRAVITY);
    feature[1] = quantize(vertical_acc / GE_GRAVITY);
    feature[2] = quantize(sqrtf(ge_max(0.0f, linear_norm * linear_norm - vertical_acc * vertical_acc)) / GE_GRAVITY);
    feature[3] = quantize(gyro_norm / 3.0f);
    feature[4] = quantize(vertical_gyro / 3.0f);
    feature[5] = quantize(sqrtf(ge_max(0.0f, gyro_norm * gyro_norm - vertical_gyro * vertical_gyro)) / 3.0f);
    high = linear_norm > 1.15f || gyro_norm > 0.85f;
    low = linear_norm > 0.65f || gyro_norm > 0.40f;
    strong = linear_norm >= GE_STRONG_ACCEL || gyro_norm >= GE_STRONG_GYRO;
    if (e->training && e->manual_training) {
        uint32_t since_press = s->timestamp_ms - e->start_ms;
        if (!e->capturing || (int32_t)since_press < 0) return;
        if (e->sample_count >= GE_MAX_SAMPLES || since_press > GE_MAX_DURATION_MS) {
            invalidate_manual_capture(e, GE_ERR_TOO_LONG);
            return;
        }
        memcpy(e->samples[e->sample_count++], feature, sizeof(feature));
        if (elapsed > since_press) elapsed = since_press;
        if (strong) { e->strong_ms += elapsed; ++e->strong_samples; }
        if (low) e->active_ms += elapsed;
        return;
    }
    if (e->streaming && !e->training) {
        stream_match(e, feature, elapsed, low);
        return;
    }
    if (!e->capturing) {
        memcpy(e->pre_roll[e->pre_head], feature, sizeof(feature));
        e->pre_head = (uint16_t)((e->pre_head + 1u) % GE_PRE_ROLL_SAMPLES);
        if (e->pre_count < GE_PRE_ROLL_SAMPLES) ++e->pre_count;
        if (!e->armed) {
            if (low) e->cooldown_ms = s->timestamp_ms;
            if (s->timestamp_ms - e->cooldown_ms >= GE_COOLDOWN_MS) e->armed = 1u;
        }
        if (!e->armed || e->pending_ready) {
            e->start_count = 0u;
            e->strong_ms = 0u;
            e->strong_samples = 0u;
            return;
        }
        e->start_count = high ? (uint8_t)(e->start_count + 1u) : 0u;
        if (!high) {
            e->strong_ms = 0u;
            e->strong_samples = 0u;
        } else if (strong) {
            e->strong_ms += elapsed;
            ++e->strong_samples;
        }
        if (e->start_count < GE_START_SAMPLES) return;
        for (i = 0u; i < e->pre_count; ++i) {
            uint16_t index = (uint16_t)((e->pre_head + GE_PRE_ROLL_SAMPLES - e->pre_count + i) % GE_PRE_ROLL_SAMPLES);
            memcpy(e->samples[i], e->pre_roll[index], sizeof(feature));
        }
        e->sample_count = e->pre_count;
        e->capturing = 1u;
        e->start_ms = s->timestamp_ms - (uint32_t)e->pre_count * 5u;
        e->active_ms = GE_START_SAMPLES * 5u;
        e->quiet_ms = 0u;
        return;
    }
    if (e->sample_count >= GE_MAX_SAMPLES || s->timestamp_ms - e->start_ms > GE_MAX_DURATION_MS) {
        reject_segment(e, GE_ERR_TOO_LONG, s->timestamp_ms - e->start_ms);
        reset_capture(e, s->timestamp_ms);
        return;
    }
    memcpy(e->samples[e->sample_count++], feature, sizeof(feature));
    if (strong) { e->strong_ms += elapsed; ++e->strong_samples; }
    if (low) { e->active_ms += elapsed; e->quiet_ms = 0u; }
    else e->quiet_ms += elapsed;
    if (e->quiet_ms >= GE_END_QUIET_MS) {
        finish_segment(e, e->last_ms - e->start_ms, 1);
        reset_capture(e, s->timestamp_ms);
    }
}

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t blob_crc(const uint8_t *data, size_t length)
{
    size_t i;
    uint8_t bit;
    uint32_t crc = 0xffffffffu;
    for (i = 0u; i < length; ++i) {
        if (i >= 12u && i < 16u) continue;
        crc ^= data[i];
        for (bit = 0u; bit < 8u; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t ge_model_export(const ge_engine_t *e, uint8_t *data, size_t capacity)
{
    uint8_t id, t;
    uint16_t i, k;
    uint8_t *p;
    if (!e || !data || capacity < GE_WIRE_SIZE) return 0u;
    memset(data, 0, GE_WIRE_SIZE);
    memcpy(data, "GDT1", 4u);
    write_u16(data + 4u, 1u);
    write_u32(data + 8u, GE_WIRE_SIZE);
    p = data + GE_WIRE_HEADER;
    for (id = 0u; id < GE_MAX_CLASSES; ++id) {
        const ge_class_t *c = &e->model.classes[id];
        if (!c->used) { p += GE_WIRE_CLASS; continue; }
        p[0] = 1u; p[1] = c->template_count;
        memcpy(p + 4u, c->name, GE_NAME_BYTES);
        write_u32(p + 20u, (uint32_t)(c->threshold * 1000000.0f + 0.5f));
        p += 24u;
        for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t) {
            write_u16(p, c->templates[t].duration_ms);
            p += 4u;
            for (i = 0u; i < GE_POINTS; ++i) {
                for (k = 0u; k < GE_FEATURES; ++k) {
                    write_u16(p, (uint16_t)c->templates[t].values[i][k]);
                    p += 2u;
                }
            }
        }
    }
    write_u32(data + 12u, blob_crc(data, GE_WIRE_SIZE));
    return GE_WIRE_SIZE;
}

static ge_status_t validate_blob(const uint8_t *data, size_t length)
{
    uint8_t id, t, k, template_count;
    uint16_t i;
    const uint8_t *p;
    if (!data || length != GE_WIRE_SIZE || memcmp(data, "GDT1", 4u) ||
        read_u16(data + 4u) != 1u || read_u16(data + 6u) != 0u ||
        read_u32(data + 8u) != GE_WIRE_SIZE) return GE_ERR_FORMAT;
    if (read_u32(data + 12u) != blob_crc(data, length)) return GE_ERR_CRC;
    p = data + GE_WIRE_HEADER;
    for (id = 0u; id < GE_MAX_CLASSES; ++id) {
        if (p[0] > 1u || read_u16(p + 2u)) return GE_ERR_FORMAT;
        if (!p[0]) {
            for (i = 0u; i < GE_WIRE_CLASS; ++i) if (p[i]) return GE_ERR_FORMAT;
            p += GE_WIRE_CLASS;
            continue;
        }
        if (!p[1] || p[1] > GE_TEMPLATES_PER_CLASS || !p[4] ||
            read_u32(p + 20u) < 55000u || read_u32(p + 20u) > 180000u) return GE_ERR_FORMAT;
        for (k = 0u; k < GE_NAME_BYTES && p[4u + k]; ++k) {
            if (p[4u + k] < 32u || p[4u + k] > 126u) return GE_ERR_FORMAT;
        }
        if (k == GE_NAME_BYTES) return GE_ERR_FORMAT;
        for (; k < GE_NAME_BYTES; ++k) if (p[4u + k]) return GE_ERR_FORMAT;
        template_count = p[1];
        p += 24u;
        for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t) {
            if (t >= template_count) {
                for (i = 0u; i < GE_WIRE_TEMPLATE; ++i) if (p[i]) return GE_ERR_FORMAT;
                p += GE_WIRE_TEMPLATE;
                continue;
            }
            if (read_u16(p) < 100u || read_u16(p) > GE_MAX_DURATION_MS || read_u16(p + 2u)) return GE_ERR_FORMAT;
            p += 4u;
            for (i = 0u; i < GE_POINTS; ++i) {
                for (k = 0u; k < GE_FEATURES; ++k) {
                    uint16_t raw = read_u16(p);
                    int32_t value = raw <= 32767u ? (int32_t)raw : (int32_t)raw - 65536;
                    if (value < -16384 || value > 16384 ||
                        ((k == 0u || k == 2u || k == 3u || k == 5u) && value < 0)) return GE_ERR_FORMAT;
                    p += 2u;
                }
            }
        }
    }
    return GE_OK;
}

ge_status_t ge_model_import(ge_engine_t *e, const uint8_t *data, size_t length)
{
    ge_status_t status;
    const uint8_t *p;
    uint8_t id, t;
    uint16_t i, k;
    if (!e) return GE_ERR_ARGUMENT;
    if (e->training || e->capturing) return GE_ERR_BUSY;
    status = validate_blob(data, length);
    if (status != GE_OK) return status;
    memset(&e->model, 0, sizeof(e->model));
    p = data + GE_WIRE_HEADER;
    for (id = 0u; id < GE_MAX_CLASSES; ++id) {
        ge_class_t *c = &e->model.classes[id];
        if (!p[0]) { p += GE_WIRE_CLASS; continue; }
        c->used = p[0]; c->template_count = p[1];
        memcpy(c->name, p + 4u, GE_NAME_BYTES);
        c->threshold = (float)read_u32(p + 20u) / 1000000.0f;
        p += 24u;
        for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t) {
            c->templates[t].duration_ms = read_u16(p);
            p += 4u;
            for (i = 0u; i < GE_POINTS; ++i) {
                for (k = 0u; k < GE_FEATURES; ++k) {
                    uint16_t raw = read_u16(p);
                    int32_t value = raw <= 32767u ? (int32_t)raw : (int32_t)raw - 65536;
                    c->templates[t].values[i][k] = (int16_t)value;
                    p += 2u;
                }
            }
        }
    }
    e->event_count = e->event_read = e->event_write = 0u;
    reset_capture(e, e->last_ms);
    reset_stream_match(e);
    return GE_OK;
}

const char *ge_status_string(ge_status_t status)
{
    static const char *const names[] = {
        "OK", "BAD ARGUMENT", "BUSY", "FULL", "TOO SHORT", "TOO LONG", "LOW QUALITY",
        "INCONSISTENT", "CONFLICT", "NOT READY", "BAD FORMAT", "BAD CRC", "SAMPLE GAP"
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[(unsigned)status] : "ERROR";
}
