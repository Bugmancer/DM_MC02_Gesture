#include "gesture_engine.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PI_F 3.14159265358979323846f
#define GRAVITY 9.80665f

static ge_engine_t engine, imported;
static uint8_t blob[GE_MODEL_BLOB_MAX], snapshot[GE_MODEL_BLOB_MAX];
static uint32_t timestamp;
static unsigned recognized, unknown, accepted, rejected, finished, ready, warned;
static uint8_t last_class;
static ge_status_t last_rejection, last_unknown_status;
static float last_training_distance, last_recognition_distance;
static unsigned previews, matched;
static uint8_t last_preview;
static ge_event_t last_warning;

static void events(ge_engine_t *e)
{
    ge_event_t event;
    while (ge_next_event(e, &event)) {
        if (event.type == GE_EVENT_RECOGNIZED) {
            ++recognized;
            last_class = event.class_id;
            last_recognition_distance = event.distance;
        }
        if (event.type == GE_EVENT_UNKNOWN) { ++unknown; last_unknown_status = event.status; }
        if (event.type == GE_EVENT_DEMO_ACCEPTED) ++accepted;
        if (event.type == GE_EVENT_DEMO_REJECTED) { ++rejected; last_rejection = event.status; }
        if (event.type == GE_EVENT_DEMO_WARNING) { ++warned; last_warning = event; }
        if (event.type == GE_EVENT_DEMO_ACCEPTED || event.type == GE_EVENT_DEMO_REJECTED)
            last_training_distance = event.distance;
        if (event.type == GE_EVENT_SEGMENT_FINISHED) ++finished;
        if (event.type == GE_EVENT_TRAIN_READY) ++ready;
        if (event.type == GE_EVENT_MATCH) {
            ++previews;
            last_preview = event.class_id;
            if (event.class_id != GE_CLASS_NONE) ++matched;
        }
    }
}

static void clear_events(void)
{
    recognized = unknown = accepted = rejected = finished = ready = warned = 0u;
    last_class = GE_CLASS_NONE;
    last_rejection = GE_OK;
    last_unknown_status = GE_OK;
    last_training_distance = last_recognition_distance = 0.0f;
    previews = matched = 0u;
    last_preview = GE_CLASS_NONE;
    memset(&last_warning, 0, sizeof(last_warning));
}

static void feed(ge_engine_t *e, float ax, float ay, float az,
                 float gx, float gy, float gz, int tilt)
{
    ge_sample_t s;
    const float c = 0.70710678118f;
    timestamp += 5u;
    s.timestamp_ms = timestamp;
    if (tilt == 2) {
        s.ax = az; s.ay = ax; s.az = ay;
        s.gx = gz; s.gy = gx; s.gz = gy;
    } else if (tilt == 3) {
        /* Rz(1.2) Ry(-0.8) Rx(0.4), an arbitrary fixed 3D holding pose. */
        s.ax = 0.252457f * ax - 0.959689f * ay + 0.123578f * az;
        s.ay = 0.649358f * ax + 0.073386f * ay - 0.756934f * az;
        s.az = 0.717356f * ax + 0.271310f * ay + 0.641709f * az;
        s.gx = 0.252457f * gx - 0.959689f * gy + 0.123578f * gz;
        s.gy = 0.649358f * gx + 0.073386f * gy - 0.756934f * gz;
        s.gz = 0.717356f * gx + 0.271310f * gy + 0.641709f * gz;
    } else if (tilt) {
        s.ax = c * ax + c * az; s.ay = ay; s.az = -c * ax + c * az;
        s.gx = c * gx + c * gz; s.gy = gy; s.gz = -c * gx + c * gz;
    } else {
        s.ax = ax; s.ay = ay; s.az = az;
        s.gx = gx; s.gy = gy; s.gz = gz;
    }
    ge_feed(e, &s);
    events(e);
}

static void idle(ge_engine_t *e, unsigned milliseconds, int tilt)
{
    unsigned i;
    for (i = 0u; i < milliseconds / 5u; ++i) feed(e, 0.0f, 0.0f, GRAVITY, 0.0f, 0.0f, 0.0f, tilt);
}

/* Exact analytic trajectories allow independent speed, intensity and initial
 * tilt changes while the same logical gesture is preserved. */
static void motion_samples(ge_engine_t *e, int kind, unsigned milliseconds, float amplitude, int tilt)
{
    unsigned i, count = milliseconds / 5u;
    for (i = 0u; i < count; ++i) {
        float phase = (float)i / (float)(count - 1u);
        float wave = sinf(2.0f * PI_F * phase);
        float ax = 0.0f, ay = 0.0f, az = GRAVITY, gz = 0.0f;
        if (kind == 0) az += amplitude * 4.5f * wave;
        if (kind == 1) gz = amplitude * 3.0f * wave;
        if (kind == 2) {
            ax = amplitude * 7.0f * sinf(4.0f * PI_F * phase);
            gz = amplitude * 2.5f * sinf(6.0f * PI_F * phase);
        }
        if (kind == 3) az += amplitude * 4.5f * fabsf(wave);
        feed(e, ax, ay, az, 0.0f, 0.0f, gz, tilt);
    }
}

static void motion(ge_engine_t *e, int kind, unsigned milliseconds, float amplitude, int tilt)
{
    motion_samples(e, kind, milliseconds, amplitude, tilt);
    idle(e, 900u, tilt);
}

static void variant_motion_samples(ge_engine_t *e, unsigned milliseconds, float return_scale)
{
    unsigned i, count = milliseconds / 5u;
    for (i = 0u; i < count; ++i) {
        float wave = sinf(2.0f * PI_F * (float)i / (float)(count - 1u));
        if (wave < 0.0f) wave *= return_scale;
        feed(e, 0.0f, 0.0f, GRAVITY + 4.5f * wave, 0.0f, 0.0f, 0.0f, 0);
    }
}

static void enroll(ge_engine_t *e, uint8_t id, const char *name, int kind)
{
    unsigned i;
    uint8_t saved = GE_CLASS_NONE;
    unsigned before = ge_class_count(e);
    assert(ge_train_begin_at(e, id, name) == GE_OK);
    assert(ge_train_confirm(e, &saved) == GE_ERR_NOT_READY);
    idle(e, 600u, 0);
    for (i = 0u; i < 3u; ++i) {
        motion(e, kind, 700u + i * 50u, 1.0f + 0.02f * (float)i, 0);
        if (ge_training_progress(e) != i + 1u)
            printf("enroll %s failed: %s, progress=%u\n", name,
                   ge_status_string(ge_training_error(e)), (unsigned)ge_training_progress(e));
        assert(ge_training_progress(e) == i + 1u);
        assert(ge_class_count(e) == before);
    }
    assert(ge_training_ready(e));
    assert(ge_train_confirm(e, &saved) == GE_OK);
    assert(saved == id);
    assert(ge_class_count(e) == before + 1u);
    idle(e, 600u, 0);
}

static uint32_t test_crc(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned b;
    for (i = 0; i < length; ++i) {
        if (i >= 12u && i < 16u) continue;
        crc ^= data[i];
        for (b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

static void repair_crc(uint8_t *data, size_t length)
{
    uint32_t crc = test_crc(data, length);
    data[12] = (uint8_t)crc; data[13] = (uint8_t)(crc >> 8);
    data[14] = (uint8_t)(crc >> 16); data[15] = (uint8_t)(crc >> 24);
}

static void test_training_and_speed(void)
{
    unsigned i;
    static const unsigned durations[] = { 450u, 700u, 1150u };
    static const float amplitudes[] = { 1.6f, 1.0f, 0.55f };
    ge_init(&engine);
    idle(&engine, 1000u, 0);
    clear_events();
    enroll(&engine, 3u, "VERTICAL", 0);
    assert(accepted == 3u && ready == 1u);
    assert(ge_train_begin_at(&engine, 3u, "OVERWRITE") == GE_ERR_CONFLICT);
    for (i = 0u; i < 3u; ++i) {
        clear_events();
        motion(&engine, 0, durations[i], amplitudes[i], 0);
        printf("speed %ums amp %.2f: recognized=%u unknown=%u\n", durations[i], amplitudes[i], recognized, unknown);
        assert(recognized == 1u && last_class == 3u && unknown == 0u);
    }
    ge_reset_stream(&engine);
    idle(&engine, 900u, 1);
    clear_events();
    motion(&engine, 0, 850u, 0.9f, 1);
    assert(recognized == 1u && last_class == 3u);
    ge_reset_stream(&engine);
    idle(&engine, 900u, 0);
}

static void test_incremental_conflict_unknown(void)
{
    unsigned before;
    clear_events();
    assert(ge_train_begin(&engine, "DUPLICATE") == GE_OK);
    idle(&engine, 600u, 0);
    motion(&engine, 0, 720u, 1.0f, 0);
    assert(rejected == 1u && last_rejection == GE_ERR_CONFLICT);
    assert(ge_training_progress(&engine) == 0u && ge_class_count(&engine) == 1u);
    ge_train_cancel(&engine);
    idle(&engine, 600u, 0);
    enroll(&engine, 5u, "YAW", 1);
    clear_events();
    motion(&engine, 0, 800u, 0.8f, 0);
    assert(recognized == 1u && last_class == 3u);
    motion(&engine, 1, 1100u, 0.65f, 0);
    assert(recognized == 2u && last_class == 5u);
    before = recognized;
    motion(&engine, 2, 900u, 1.0f, 0);
    motion(&engine, 3, 750u, 1.0f, 0);
    assert(recognized == before && unknown >= 2u);
    assert(ge_class_count(&engine) == 2u);
}

static void test_noise_continuous_and_short(void)
{
    unsigned i;
    clear_events();
    /* Twenty minutes of subthreshold vibration and drift must produce no events. */
    for (i = 0u; i < 240000u; ++i) {
        float noise = 0.15f * sinf((float)i * 0.37f);
        feed(&engine, noise, noise * 0.5f, GRAVITY + noise, 0.0f, 0.0f, noise * 0.2f, 0);
    }
    assert(recognized == 0u && finished == 0u);
    /* Sustained motion cannot become a sequence of accepted templates. */
    for (i = 0u; i < 1600u; ++i) feed(&engine, 4.0f, 0.0f, GRAVITY, 0.0f, 0.0f, 2.0f, 0);
    idle(&engine, 1000u, 0);
    assert(recognized == 0u && unknown == 1u && finished == 1u);
    assert(ge_train_begin(&engine, "SHORT") == GE_OK);
    idle(&engine, 600u, 0);
    clear_events();
    for (i = 0u; i < 10u; ++i) feed(&engine, 0.0f, 0.0f, GRAVITY + 4.0f, 0.0f, 0.0f, 0.0f, 0);
    idle(&engine, 900u, 0);
    assert(rejected == 1u && ge_training_progress(&engine) == 0u);
    assert(last_rejection == GE_ERR_TOO_SHORT);
    ge_train_cancel(&engine);
}

static void test_disabled_and_reset(void)
{
    unsigned i;
    idle(&engine, 700u, 0);
    ge_set_recognition(&engine, 0);
    clear_events();
    motion(&engine, 0, 750u, 1.0f, 0);
    assert(finished == 1u && recognized == 0u && unknown == 0u);
    assert(ge_train_begin(&engine, "DISABLED") == GE_OK);
    idle(&engine, 600u, 0);
    motion(&engine, 2, 800u, 1.0f, 0);
    assert(ge_training_progress(&engine) == 1u);
    ge_reset_stream(&engine);
    assert(ge_training_progress(&engine) == 1u);
    idle(&engine, 1000u, 0);
    motion(&engine, 3, 800u, 1.0f, 0);
    assert(ge_training_progress(&engine) == 1u && last_rejection == GE_ERR_INCONSISTENT);
    motion(&engine, 2, 850u, 0.95f, 0);
    motion(&engine, 2, 900u, 1.02f, 0);
    assert(ge_training_ready(&engine) && ge_class_count(&engine) == 2u);
    clear_events();
    motion(&engine, 3, 800u, 1.0f, 0);
    assert(finished == 0u && ge_training_progress(&engine) == 3u);
    ge_train_cancel(&engine);
    assert(!ge_training_ready(&engine) && ge_class_count(&engine) == 2u);
    ge_set_recognition(&engine, 1);
    idle(&engine, 700u, 0);
    clear_events();
    for (i = 0u; i < 25u; ++i) feed(&engine, 0.0f, 0.0f, GRAVITY + 3.0f, 0.0f, 0.0f, 0.0f, 0);
    timestamp += 100u;
    feed(&engine, 0.0f, 0.0f, GRAVITY, 0.0f, 0.0f, 0.0f, 0);
    idle(&engine, 1000u, 0);
    assert(recognized == 0u && unknown == 1u);
    timestamp = UINT32_MAX - 200u;
    ge_reset_stream(&engine);
    idle(&engine, 1000u, 0);
    clear_events();
    motion(&engine, 0, 700u, 1.0f, 0);
    assert(recognized == 1u && last_class == 3u);
}

static void test_serialization(void)
{
    size_t length = ge_model_export(&engine, blob, sizeof(blob));
    size_t again;
    assert(length > 0u && length <= GE_MODEL_BLOB_MAX);
    assert(ge_model_export(&engine, snapshot, length - 1u) == 0u);
    ge_init(&imported);
    assert(ge_model_import(&imported, blob, length) == GE_OK);
    again = ge_model_export(&imported, snapshot, sizeof(snapshot));
    assert(again == length && memcmp(blob, snapshot, length) == 0);
    assert(ge_class_count(&imported) == 2u);
    assert(ge_model_import(&imported, blob, length - 1u) == GE_ERR_FORMAT);
    blob[length - 30u] ^= 1u;
    assert(ge_model_import(&imported, blob, length) == GE_ERR_CRC);
    assert(ge_class_count(&imported) == 2u);
    memcpy(blob, snapshot, length);
    blob[16u] = 2u;
    repair_crc(blob, length);
    assert(ge_model_import(&imported, blob, length) == GE_ERR_FORMAT);
    assert(ge_class_count(&imported) == 2u);
    memcpy(blob, snapshot, length);
    /* A valid CRC must not let an attacker-controlled class threshold through. */
    blob[16u + 3u * 1764u + 20u] = 0xffu;
    blob[16u + 3u * 1764u + 21u] = 0xffu;
    blob[16u + 3u * 1764u + 22u] = 0xffu;
    blob[16u + 3u * 1764u + 23u] = 0x7fu;
    repair_crc(blob, length);
    assert(ge_model_import(&imported, blob, length) == GE_ERR_FORMAT);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length);
    assert(memcmp(blob, snapshot, length) == 0);
    ge_reset_stream(&imported);
    idle(&imported, 1000u, 0);
    clear_events();
    motion(&imported, 0, 1050u, 0.7f, 0);
    assert(recognized == 1u && last_class == 3u);
    assert(ge_class_delete(&imported, 3u) == GE_OK);
    assert(ge_class_count(&imported) == 1u && ge_class_get(&imported, 5u));
    assert(ge_class_delete(&imported, 3u) == GE_ERR_ARGUMENT);
    printf("model bytes=%zu engine bytes=%zu\n", length, sizeof(ge_engine_t));
}

static void test_horizontal_rotation_gravity(void)
{
    unsigned i;
    float theta = 0.0f, maximum_error = 0.0f;
    ge_reset_stream(&engine);
    idle(&engine, 1000u, 0);
    clear_events();
    /* Rigid pitch about body X: measured gravity is (0, g sin(t), g cos(t)).
     * No translational acceleration is present. The gyro sign must agree. */
    for (i = 0u; i < 400u; ++i) {
        float omega = 2.4f * sinf(2.0f * PI_F * (float)i / 399.0f);
        float actual_y, actual_z, error;
        theta += omega * 0.005f;
        actual_y = GRAVITY * sinf(theta);
        actual_z = GRAVITY * cosf(theta);
        feed(&engine, 0.0f, actual_y, actual_z, omega, 0.0f, 0.0f, 0);
        error = sqrtf(engine.gravity[0] * engine.gravity[0] +
                      (engine.gravity[1] - actual_y) * (engine.gravity[1] - actual_y) +
                      (engine.gravity[2] - actual_z) * (engine.gravity[2] - actual_z));
        if (error > maximum_error) maximum_error = error;
    }
    idle(&engine, 1000u, 0);
    assert(maximum_error < 0.04f);
    assert(recognized == 0u && unknown == 1u);
    printf("rigid horizontal-axis rotation: max gravity error=%.6f m/s2\n", maximum_error);
}

static void test_small_motion_boundary(void)
{
    unsigned i;
    static const float amplitudes[] = { 0.15f, 0.28f, 0.32f, 0.40f };
    for (i = 0u; i < sizeof(amplitudes) / sizeof(amplitudes[0]); ++i) {
        clear_events();
        motion(&engine, 0, 750u, amplitudes[i], 0);
        printf("small-motion probe: peak=%.3f m/s2 recognized=%u unknown=%u segments=%u\n",
               amplitudes[i] * 4.5f, recognized, unknown, finished);
        if (i == 0u) assert(recognized == 0u && finished == 0u);
        else assert(recognized == 0u && unknown == 1u && finished == 1u && last_unknown_status == GE_ERR_QUALITY);
    }
    clear_events();
    motion(&engine, 1, 750u, 0.35f, 0);
    assert(recognized == 0u && unknown == 1u && last_unknown_status == GE_ERR_QUALITY);
    clear_events();
    for (i = 0u; i < 150u; ++i) {
        float acceleration = 1.8f * sinf(2.0f * PI_F * (float)i / 149.0f);
        if (i >= 35u && i < 38u) acceleration = 4.5f;
        feed(&engine, 0.0f, 0.0f, GRAVITY + acceleration, 0.0f, 0.0f, 0.0f, 0);
    }
    idle(&engine, 900u, 0);
    assert(recognized == 0u && unknown == 1u && last_unknown_status == GE_ERR_QUALITY);
    assert(ge_train_begin(&engine, "WEAK DEMO") == GE_OK);
    idle(&engine, 600u, 0);
    clear_events();
    motion(&engine, 0, 750u, 0.40f, 0);
    assert(rejected == 1u && last_rejection == GE_ERR_QUALITY && ge_training_progress(&engine) == 0u);
    ge_train_cancel(&engine);
}

static void test_eight_hour_stream(void)
{
    const uint32_t total_samples = 8u * 60u * 60u * 200u;
    const uint32_t period_samples = 60u * 200u;
    uint32_t i, rng = 0x6835a20du, wraps = 0u, previous_ms;
    unsigned vertical_count = 0u, yaw_count = 0u, false_accepts = 0u;
    uint16_t maximum_buffered = 0u;
    size_t model_length;
    clock_t begin = clock();
    assert(ge_class_count(&engine) == 2u);
    model_length = ge_model_export(&engine, snapshot, sizeof(snapshot));
    assert(model_length > 0u);
    timestamp = UINT32_MAX - 1375u;
    ge_reset_stream(&engine);
    clear_events();
    for (i = 0u; i < total_samples; ++i) {
        uint32_t offset = i % period_samples;
        uint32_t cycle = i / period_samples;
        float ax = 0.0f, ay = 0.0f, az = GRAVITY, gz = 0.0f;
        float phase, wave, noise;
        unsigned old_recognized = recognized;
        int expected = -1;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        noise = ((float)(rng & 0xffffu) / 65535.0f - 0.5f) * 0.08f;
        if (offset >= 200u && offset < 350u) {
            phase = (float)(offset - 200u) / 149.0f;
            az += 4.5f * sinf(2.0f * PI_F * phase);
        } else if (offset >= 1000u && offset < 1150u) {
            phase = (float)(offset - 1000u) / 149.0f;
            gz = 3.0f * sinf(2.0f * PI_F * phase);
        } else if (offset >= 1800u && offset < 1950u) {
            phase = (float)(offset - 1800u) / 149.0f;
            ax = 7.0f * sinf(4.0f * PI_F * phase);
            gz = 2.5f * sinf(6.0f * PI_F * phase);
        } else if (offset >= 2600u && offset < 2750u) {
            phase = (float)(offset - 2600u) / 149.0f;
            az += 1.26f * sinf(2.0f * PI_F * phase);
        } else if (offset >= 5400u && offset < 5550u) {
            phase = (float)(offset - 5400u) / 149.0f;
            az += 1.44f * sinf(2.0f * PI_F * phase);
        } else if (offset >= 6200u && offset < 6350u) {
            phase = (float)(offset - 6200u) / 149.0f;
            az += 1.80f * sinf(2.0f * PI_F * phase);
        } else if (offset >= 3200u && offset < 4800u) {
            ax = 4.0f; gz = 2.0f;
        } else if ((offset >= 11900u && cycle < 479u) || (offset < 50u && cycle > 0u)) {
            uint32_t across = offset >= 11900u ? offset - 11900u : offset + 100u;
            phase = (float)across / 149.0f;
            wave = sinf(2.0f * PI_F * phase);
            az += 4.5f * wave;
        }
        if ((offset >= 200u && offset < 550u) ||
            (offset >= 11900u && cycle < 479u) || (offset < 180u && cycle > 0u)) expected = 3;
        if (offset >= 1000u && offset < 1350u) expected = 5;
        previous_ms = timestamp;
        feed(&engine, ax + noise, ay + noise * 0.25f, az + noise * 0.5f,
             0.0f, 0.0f, gz + noise * 0.05f, 0);
        if (timestamp < previous_ms) ++wraps;
        if (engine.sample_count > maximum_buffered) maximum_buffered = engine.sample_count;
        assert(engine.sample_count <= GE_MAX_SAMPLES && engine.event_count <= 4u);
        if (recognized != old_recognized) {
            if ((int)last_class != expected) ++false_accepts;
            else if (last_class == 3u) ++vertical_count;
            else if (last_class == 5u) ++yaw_count;
        }
    }
    assert(wraps == 1u);
    assert(vertical_count == 959u && yaw_count == 480u);
    assert(false_accepts == 0u);
    assert(unknown == 2400u && finished == 3839u);
    assert(ge_model_export(&engine, blob, sizeof(blob)) == model_length);
    assert(memcmp(blob, snapshot, model_length) == 0);
    printf("8h synthetic stream: samples=%lu wraps=%lu known=%u unknown=%u false_accepts=%u segments=%u\n",
           (unsigned long)total_samples, (unsigned long)wraps, recognized, unknown, false_accepts, finished);
    printf("8h synthetic stream: fixed engine=%zu B blob=%zu B max_buffered=%u host_cpu=%.3fs\n",
           sizeof(engine), model_length, (unsigned)maximum_buffered, (double)(clock() - begin) / CLOCKS_PER_SEC);
}

static void prepare_manual(ge_engine_t *e)
{
    ge_init(e);
    ge_set_manual_training(e, 1);
    assert(ge_train_begin_at(e, 2u, "MANUAL") == GE_OK);
    idle(e, 1000u, 0);
    clear_events();
}

static void test_manual_without_stationary_calibration(void)
{
    static const float initial_acceleration[] = { GRAVITY, GRAVITY + 4.0f, GRAVITY - 4.0f };
    unsigned i;
    ge_sample_t invalid;
    for (i = 0u; i < sizeof(initial_acceleration) / sizeof(initial_acceleration[0]); ++i) {
        ge_init(&imported);
        ge_set_manual_training(&imported, 1);
        assert(ge_train_begin_at(&imported, 2u, "UNSETTLED") == GE_OK);
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        feed(&imported, 0.0f, 0.0f, initial_acceleration[i], 0.0f, 0.0f, 1.5f, 0);
        assert(imported.initialized == 1u);
        assert(imported.sample_count == 1u);
        feed(&imported, 0.0f, 0.0f, GRAVITY + 3.0f, 0.0f, 0.0f, 1.5f, 0);
        assert(imported.sample_count == 2u && imported.initialized == 1u);
        assert(imported.samples[1][0] > 200 && imported.samples[1][3] > 400);
        idle(&imported, 400u, 0);
        assert(imported.sample_count == 82u && imported.initialized == 1u);
        motion_samples(&imported, 0, 700u, 1.0f, 0);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        assert(ge_training_progress(&imported) == 1u && imported.initialized == 1u);
    }
    ge_train_cancel(&imported);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(finished == 0u); /* Unsettled manual capture cannot arm automatic detection. */
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(finished == 1u && unknown == 1u);

    assert(ge_train_begin_at(&imported, 2u, "FRESHNESS") == GE_OK);
    ge_reset_stream(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    memset(&invalid, 0, sizeof(invalid));
    invalid.timestamp_ms = timestamp += 5u;
    ge_feed(&imported, &invalid);
    assert(imported.sample_count == 0u);
    assert(ge_train_capture_end(&imported, timestamp) != GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    invalid.timestamp_ms = timestamp += 5u;
    invalid.az = NAN;
    ge_feed(&imported, &invalid);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_QUALITY);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    invalid.timestamp_ms = timestamp += 5u;
    invalid.az = 250.0f;
    ge_feed(&imported, &invalid);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_QUALITY);
    feed(&imported, 0.0f, 0.0f, GRAVITY + 4.0f, 0.0f, 0.0f, 1.5f, 0);
    assert(imported.initialized == 1u);
    timestamp += 31u;
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    assert(!imported.initialized);
    ge_reset_stream(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    feed(&imported, 0.0f, 0.0f, GRAVITY - 4.0f, 0.0f, 0.0f, 1.5f, 0);
    feed(&imported, 0.0f, 0.0f, GRAVITY + 3.0f, 0.0f, 0.0f, 1.5f, 0);
    assert(imported.sample_count == 2u);
    ge_train_cancel(&imported);

    ge_init(&imported);
    ge_set_manual_training(&imported, 1);
    feed(&imported, 0.0f, 0.0f, GRAVITY + 4.0f, 0.0f, 0.0f, 1.5f, 0);
    assert(imported.initialized == 0u); /* Recognition keeps its original reference gate. */
    puts("Manual learning: press before first sample, moving/stale starts and invalid data passed.");
}

static void test_manual_press_release_boundaries(void)
{
    unsigned count;
    ge_init(&imported);
    assert(ge_train_begin(&imported, "AUTOMATIC") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_ERR_NOT_READY);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_NOT_READY);
    prepare_manual(&imported);
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(!ge_training_capturing(&imported));
    assert(imported.sample_count == 0u && imported.pre_count == 0u);
    assert(accepted == 0u && rejected == 0u && finished == 0u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_ERR_BUSY);
    idle(&imported, 200u, 0);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    idle(&imported, 300u, 0);
    assert(ge_training_capturing(&imported));
    assert(ge_training_progress(&imported) == 0u && finished == 0u);
    assert(imported.sample_count == 240u);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(!ge_training_capturing(&imported));
    assert(accepted == 1u && finished == 1u && rejected == 0u);
    assert(imported.pending[0].duration_ms == 1200u);
    assert(imported.pending[0].values[0][0] == 0);
    assert(imported.pending[0].values[GE_POINTS - 1u][0] == 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_NOT_READY);
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(accepted == 1u && imported.sample_count == 0u);

    assert(ge_train_capture_begin(&imported, timestamp + 10u) == GE_OK);
    feed(&imported, 0.0f, 0.0f, GRAVITY + 4.0f, 0.0f, 0.0f, 0.0f, 0);
    assert(imported.sample_count == 0u); /* Queued sample predates the key edge. */
    feed(&imported, 0.0f, 0.0f, GRAVITY, 0.0f, 0.0f, 0.0f, 0);
    assert(imported.sample_count == 1u);
    ge_train_cancel(&imported);
    assert(!ge_training_capturing(&imported) && ge_training_progress(&imported) == 0u);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_NOT_READY);
    count = accepted;
    idle(&imported, 1000u, 0);
    assert(accepted == count);
}

static void test_manual_rejections_and_reset(void)
{
    unsigned i;
    ge_sample_t invalid;
    prepare_manual(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_TOO_SHORT);
    events(&imported);
    assert(rejected == 1u && accepted == 0u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 175u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_TOO_SHORT);
    events(&imported);
    assert(rejected == 2u && accepted == 0u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 0.40f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_QUALITY);
    events(&imported);
    assert(rejected == 3u && accepted == 0u);

    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    timestamp += 40u; /* Release also detects missing samples at the tail. */
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_SAMPLE_GAP);
    events(&imported);
    assert(rejected == 4u && accepted == 0u);
    idle(&imported, 1000u, 0);
    clear_events();
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 200u, 1.0f, 0);
    timestamp += 50u;
    idle(&imported, 1000u, 0);
    assert(ge_training_capturing(&imported) && finished == 0u && rejected == 0u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_ERR_BUSY);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_SAMPLE_GAP);
    events(&imported);
    assert(rejected == 1u && accepted == 0u);
    idle(&imported, 1000u, 0);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 200u, 1.0f, 0);
    memset(&invalid, 0, sizeof(invalid));
    invalid.timestamp_ms = timestamp += 5u;
    invalid.ax = NAN;
    ge_feed(&imported, &invalid);
    idle(&imported, 500u, 0);
    assert(ge_training_capturing(&imported) && rejected == 1u);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_QUALITY);
    events(&imported);
    assert(rejected == 2u && accepted == 0u);

    idle(&imported, 1000u, 0);
    clear_events();
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(accepted == 1u && ge_training_progress(&imported) == 1u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 200u, 1.0f, 0);
    ge_reset_stream(&imported);
    assert(!ge_training_capturing(&imported));
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_NOT_READY);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    ge_reset_stream(&imported);
    idle(&imported, 1000u, 0);
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(accepted == 1u && ge_training_progress(&imported) == 1u);

    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    idle(&imported, 2500u, 0);
    assert(ge_training_capturing(&imported) && accepted == 1u && rejected == 0u);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_ERR_BUSY);
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_TOO_LONG);
    events(&imported);
    assert(rejected == 1u && ge_training_progress(&imported) == 1u);

    idle(&imported, 1000u, 0);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    memset(&invalid, 0, sizeof(invalid));
    invalid.az = GRAVITY + 4.0f;
    for (i = 0u; i < GE_MAX_SAMPLES; ++i) {
        invalid.timestamp_ms = ++timestamp;
        ge_feed(&imported, &invalid);
    }
    assert(imported.sample_count == GE_MAX_SAMPLES && imported.capture_error == GE_OK);
    invalid.timestamp_ms = ++timestamp;
    ge_feed(&imported, &invalid);
    assert(imported.sample_count == GE_MAX_SAMPLES && ge_training_capturing(&imported));
    assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_TOO_LONG);
    events(&imported);
    assert(rejected == 2u && ge_training_progress(&imported) == 1u);

    prepare_manual(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 2400u, 1.0f, 0);
    assert(imported.sample_count == 480u && ge_training_capturing(&imported));
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(imported.pending[0].duration_ms == 2400u);
    prepare_manual(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 180u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(imported.pending[0].duration_ms == 180u);
}

static void test_manual_training_then_automatic_recognition(void)
{
    unsigned i;
    uint8_t saved = GE_CLASS_NONE;
    size_t length;
    prepare_manual(&imported);
    for (i = 0u; i < GE_TEMPLATES_PER_CLASS; ++i) {
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        idle(&imported, 50u, 0);
        motion_samples(&imported, 0, 700u + i * 50u, 1.0f + (float)i * 0.02f, 0);
        idle(&imported, 50u, 0);
        assert(ge_training_progress(&imported) == i);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        events(&imported);
        assert(ge_training_progress(&imported) == i + 1u);
        idle(&imported, 500u, 0);
        if (i == 0u) {
            assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
            motion_samples(&imported, 2, 800u, 1.0f, 0);
            assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_INCONSISTENT);
            events(&imported);
            assert(ge_training_progress(&imported) == 1u);
            idle(&imported, 500u, 0);
        }
    }
    assert(ready == 1u && accepted == 3u && ge_training_ready(&imported));
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 750u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(ge_training_progress(&imported) == 4u && imported.pending_count == 3u);
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 2u);
    length = ge_model_export(&imported, blob, sizeof(blob));
    assert(length > 0u);
    assert(ge_model_import(&imported, blob, length) == GE_OK);
    idle(&imported, 700u, 0);
    clear_events();
    motion(&imported, 0, 450u, 1.6f, 0);
    motion(&imported, 0, 750u, 1.0f, 0);
    motion(&imported, 0, 1150u, 0.55f, 0);
    assert(recognized == 3u && unknown == 0u && last_class == 2u);
    motion(&imported, 2, 800u, 1.0f, 0);
    assert(recognized == 3u && unknown == 1u);
    motion(&imported, 3, 750u, 1.0f, 0);
    motion(&imported, 1, 750u, 1.0f, 0);
    assert(recognized == 3u && unknown == 3u);
    motion(&imported, 0, 750u, 0.4f, 0);
    assert(recognized == 3u && unknown == 4u && last_unknown_status == GE_ERR_QUALITY);
    assert(ge_train_begin_at(&imported, 4u, "DUPLICATE") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(ge_training_progress(&imported) == 1u && ge_class_count(&imported) == 1u);
    assert(warned == 1u && last_warning.class_id == 2u && last_warning.status == GE_ERR_CONFLICT);
    ge_train_cancel(&imported);
    assert(ge_train_begin_at(&imported, 5u, "MANUAL YAW") == GE_OK);
    for (i = 0u; i < GE_TEMPLATES_PER_CLASS; ++i) {
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        idle(&imported, 50u, 0);
        motion_samples(&imported, 1, 700u + i * 50u, 1.0f, 0);
        idle(&imported, 50u, 0);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        events(&imported);
        idle(&imported, 500u, 0);
    }
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 5u);
    idle(&imported, 700u, 0);
    clear_events();
    motion(&imported, 0, 800u, 0.8f, 0);
    assert(recognized == 1u && last_class == 2u);
    motion(&imported, 1, 1100u, 0.65f, 0);
    assert(recognized == 2u && last_class == 5u);
    motion(&imported, 2, 800u, 1.0f, 0);
    motion(&imported, 3, 750u, 1.0f, 0);
    motion(&imported, 1, 750u, 0.35f, 0);
    assert(recognized == 2u && unknown == 3u && last_unknown_status == GE_ERR_QUALITY);
    ge_reset_stream(&imported);
    idle(&imported, 900u, 1);
    clear_events();
    motion(&imported, 0, 850u, 0.9f, 1);
    assert(recognized == 1u && last_class == 2u);
    motion(&imported, 1, 850u, 0.9f, 1);
    assert(recognized == 2u && last_class == 5u);

    prepare_manual(&imported);
    timestamp = UINT32_MAX - 500u;
    ge_reset_stream(&imported);
    idle(&imported, 400u, 0);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(imported.pending[0].duration_ms == 700u && ge_training_progress(&imported) == 1u);
    puts("Manual learning: key boundaries, failures, save, automatic recognition passed.");
}

static void test_manual_training_variation(void)
{
    static const float scales[] = { 1.0f, 0.40f, 1.50f };
    static const unsigned durations[] = { 700u, 450u, 900u };
    unsigned i;
    uint8_t saved;
    float largest_distance = 0.0f;
    ge_init(&imported);
    idle(&imported, 1000u, 0);
    assert(ge_train_begin(&imported, "AUTOMATIC") == GE_OK);
    idle(&imported, 500u, 0);
    clear_events();
    variant_motion_samples(&imported, 700u, scales[0]);
    idle(&imported, 900u, 0);
    assert(accepted == 1u);
    variant_motion_samples(&imported, 700u, scales[1]);
    idle(&imported, 900u, 0);
    printf("Automatic enrollment variation: distance=%.6f status=%s\n",
           last_training_distance, ge_status_string(last_rejection));
    assert(rejected == 1u && last_rejection == GE_ERR_INCONSISTENT);
    assert(last_training_distance > (0.180f - 0.025f) / 1.6f);

    prepare_manual(&imported);
    for (i = 0u; i < GE_TEMPLATES_PER_CLASS; ++i) {
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        idle(&imported, 40u, 0);
        variant_motion_samples(&imported, durations[i], scales[i]);
        idle(&imported, 40u, 0);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        events(&imported);
        if (last_training_distance > largest_distance) largest_distance = last_training_distance;
        printf("Manual enrollment variation %u: worst distance=%.6f threshold=%.6f\n",
               i + 1u, last_training_distance, imported.pending_threshold);
        assert(ge_training_progress(&imported) == i + 1u);
        idle(&imported, 500u, 0);
    }
    assert(largest_distance > (0.180f - 0.025f) / 1.6f && largest_distance <= 0.300f);
    assert(imported.pending_threshold <= 0.180f && ge_training_ready(&imported));
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 2u);
    /* Reuse the continuous-stream scenarios with the broader manual model. */
    engine.model.classes[3u] = imported.model.classes[2u];
    idle(&imported, 700u, 0);
    clear_events();
    for (i = 0u; i < GE_TEMPLATES_PER_CLASS; ++i) {
        variant_motion_samples(&imported, durations[i], scales[i]);
        idle(&imported, 900u, 0);
        printf("Manual variation automatic recognition %u: distance=%.6f recognized=%u unknown=%u\n",
               i + 1u, last_recognition_distance, recognized, unknown);
        assert(recognized == i + 1u && last_class == 2u && unknown == 0u);
    }
    motion(&imported, 1, 750u, 1.0f, 0);
    motion(&imported, 2, 800u, 1.0f, 0);
    motion(&imported, 3, 750u, 1.0f, 0);
    motion(&imported, 0, 750u, 0.4f, 0);
    assert(recognized == 3u && unknown == 4u && last_unknown_status == GE_ERR_QUALITY);
    assert(ge_train_begin_at(&imported, 4u, "DUPLICATE") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    variant_motion_samples(&imported, 650u, 0.70f);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(warned == 1u && last_warning.class_id == 2u && ge_training_ready(&imported));

    prepare_manual(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    for (i = 1u; i <= 3u; ++i) {
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        motion_samples(&imported, (int)i, 700u, 1.0f, 0);
        assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_INCONSISTENT);
        events(&imported);
        assert(last_training_distance > 0.300f && ge_training_progress(&imported) == 1u);
    }
    puts("Continuous-stream regression with the variable manual class:");
    test_eight_hour_stream();
    puts("Manual enrollment tolerance: moderate variants accepted; conflicts warn; unrelated and weak motion rejected.");
}

static void test_manual_similarity_warning_and_ambiguity(void)
{
    ge_class_t original;
    ge_event_t event;
    uint8_t saved;
    unsigned i;
    size_t length;
    static const ge_event_type_t first_demo_events[] = {
        GE_EVENT_SEGMENT_FINISHED, GE_EVENT_DEMO_ACCEPTED,
        GE_EVENT_DEMO_WARNING, GE_EVENT_TRAIN_READY
    };
    prepare_manual(&imported);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 2u);
    original = imported.model.classes[2];

    /* The old broad boundary blocked this valid, visibly different waveform. */
    assert(ge_train_begin_at(&imported, 0u, "NEARBY") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    variant_motion_samples(&imported, 700u, 0.40f);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(warned == 1u && rejected == 0u && last_warning.class_id == 2u);
    assert(last_warning.training_count == 1u && last_warning.status == GE_ERR_CONFLICT);
    assert(last_warning.distance > 0.05f && last_warning.distance < last_warning.second_distance);
    assert(ge_training_error(&imported) == GE_OK && ge_training_ready(&imported));
    clear_events();
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    variant_motion_samples(&imported, 700u, 0.65f);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    events(&imported);
    assert(accepted == 1u && warned == 1u && ready == 0u && rejected == 0u);
    assert(last_warning.class_id == 2u && last_warning.training_count == 2u);
    assert(ge_training_progress(&imported) == 2u && ge_training_ready(&imported));
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 0u);
    assert(!memcmp(&original, &imported.model.classes[2], sizeof(original)));
    length = ge_model_export(&imported, snapshot, sizeof(snapshot));
    assert(length > 0u && ge_model_import(&imported, snapshot, length) == GE_OK);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length && !memcmp(snapshot, blob, length));
    assert(ge_train_begin_at(&imported, 2u, "OVERWRITE") == GE_ERR_CONFLICT);

    /* A closer class later in slot order must be identified in the warning. */
    ge_reset_stream(&imported);
    idle(&imported, 1000u, 0);
    assert(ge_train_begin_at(&imported, 5u, "DUPLICATE") == GE_OK);
    clear_events();
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(imported.event_count == 4u);
    for (i = 0u; i < sizeof(first_demo_events) / sizeof(first_demo_events[0]); ++i) {
        assert(ge_next_event(&imported, &event) && event.type == first_demo_events[i]);
        if (event.type == GE_EVENT_DEMO_WARNING) {
            assert(event.class_id == 2u && event.training_count == 1u);
            assert(event.status == GE_ERR_CONFLICT && event.distance == 0.0f);
            assert(event.second_distance > event.distance);
        }
    }
    assert(!ge_next_event(&imported, &event));
    assert(ge_training_progress(&imported) == 1u && ge_training_ready(&imported));
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 5u);
    assert(!memcmp(&original, &imported.model.classes[2], sizeof(original)));
    assert(!memcmp(original.templates, imported.model.classes[5].templates, sizeof(original.templates)));
    assert(ge_class_delete(&imported, 0u) == GE_OK);
    length = ge_model_export(&imported, snapshot, sizeof(snapshot));
    assert(length > 0u && ge_model_import(&imported, snapshot, length) == GE_OK);

    /* Neither finalized recognition nor live RGB previews may break the tie. */
    ge_reset_stream(&imported);
    idle(&imported, 1000u, 0);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(unknown == 1u && recognized == 0u && matched == 0u);
    ge_set_streaming_recognition(&imported, 1);
    ge_reset_stream(&imported);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(previews > 0u && recognized == 0u && matched == 0u && unknown == 1u);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length && !memcmp(snapshot, blob, length));

    ge_set_streaming_recognition(&imported, 0);
    ge_set_manual_training(&imported, 0);
    ge_reset_stream(&imported);
    idle(&imported, 1000u, 0);
    assert(ge_train_begin_at(&imported, 4u, "AUTO DUPLICATE") == GE_OK);
    idle(&imported, 600u, 0);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(rejected == 1u && last_rejection == GE_ERR_CONFLICT && warned == 0u);
    assert(ge_training_progress(&imported) == 0u && !ge_training_ready(&imported));
    ge_train_cancel(&imported);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length && !memcmp(snapshot, blob, length));
    puts("Manual similar classes: accepted with closest-slot warning; duplicate ties reject both recognition modes.");
}

static void test_live_matching(void)
{
    unsigned i, j;
    uint8_t saved;
    size_t length;
    static const unsigned durations[] = {450u, 700u, 1150u};
    static const float amplitudes[] = {1.4f, 1.0f, 0.55f};
    ge_init(&imported);
    ge_set_manual_training(&imported, 1);
    ge_set_streaming_recognition(&imported, 1);
    assert(ge_train_begin_at(&imported, 2u, "ONE TAKE") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 0, 700u, 1.0f, 1);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(ge_training_progress(&imported) == 1u && ge_training_ready(&imported));
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 2u);
    assert(imported.model.classes[2].template_count == 1u);
    length = ge_model_export(&imported, blob, sizeof(blob));
    assert(length && ge_model_import(&imported, blob, length) == GE_OK);
    for (i = 0u; i < 4u; ++i) {
        for (j = 0u; j < 3u; ++j) {
            ge_reset_stream(&imported);
            clear_events();
            motion_samples(&imported, 0, durations[j], amplitudes[j], (int)i);
            printf("Live pose=%u speed=%u: in-motion previews=%u matches=%u actions=%u\n",
                   i, durations[j], previews, matched, recognized);
            idle(&imported, 300u, (int)i);
            printf("Live +300ms matches=%u actions=%u\n", matched, recognized);
            assert(matched > 0u && recognized == 1u && last_class == 2u);
            idle(&imported, 600u, (int)i);
            assert(recognized == 1u && finished == 0u);
        }
    }
    /* A prefix followed by unrelated continuing movement cannot trigger. */
    ge_reset_stream(&imported);
    clear_events();
    for (i = 0u; i < 70u; ++i) {
        float wave = sinf(2.0f * PI_F * (float)i / 139.0f);
        feed(&imported, 0.0f, 0.0f, GRAVITY + 4.5f * wave, 0.0f, 0.0f, 0.0f, 0);
    }
    motion_samples(&imported, 2, 2000u, 1.0f, 0);
    assert(recognized == 0u && matched == 0u);
    idle(&imported, 500u, 0);
    clear_events();
    motion_samples(&imported, 0, 700u, 0.4f, 0);
    idle(&imported, 500u, 0);
    assert(recognized == 0u && matched == 0u);

    /* Each complete gesture can trigger without a quiet gap; overlapping
     * windows of one occurrence cannot create repeated actions. */
    ge_reset_stream(&imported);
    assert(ge_train_begin_at(&imported, 5u, "YAW ONE") == GE_OK);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
    motion_samples(&imported, 1, 700u, 1.0f, 0);
    assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 5u);
    ge_reset_stream(&imported);
    clear_events();
    motion_samples(&imported, 0, 700u, 1.0f, 0);
    assert(matched > 0u && recognized == 1u && last_class == 2u);
    for (i = 0u; i < 3u; ++i) {
        motion_samples(&imported, 1, 700u, 1.0f, 0);
        printf("Live continuous repeat %u: actions=%u class=%u\n", i + 1u, recognized, last_class);
        assert(recognized == i + 2u && last_class == 5u);
    }
    /* Discovery suppresses reused windows while looking for a new occurrence;
     * its preview may already be clear after the confirmed action. */
    assert(last_preview == 5u || last_preview == GE_CLASS_NONE);
    idle(&imported, 500u, 0);
    assert(last_preview == GE_CLASS_NONE);
    motion(&imported, 1, 700u, 1.0f, 0);
    assert(recognized == 5u && last_class == 5u);
    puts("Live matching: arbitrary holding poses, one-take save, speed, prefix/weak rejection and continuous complete actions passed.");
}

static void test_live_twenty_minute_stream(void)
{
    uint32_t i, rng = 0x73219453u;
    unsigned positives = 0u, false_accepts = 0u;
    size_t length = ge_model_export(&imported, snapshot, sizeof(snapshot));
    clock_t started = clock();
    ge_reset_stream(&imported);
    timestamp = UINT32_MAX - 2775u;
    clear_events();
    for (i = 0u; i < 20u * 60u * 200u; ++i) {
        uint32_t step = i % 2000u;
        float ax = 0.0f, az = GRAVITY, gz = 0.0f, noise;
        unsigned before = recognized;
        int expected = -1;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        noise = ((float)(rng & 0xffffu) / 65535.0f - 0.5f) * 0.10f;
        if (step >= 100u && step < 240u) az += 4.5f * sinf(2.0f * PI_F * (float)(step - 100u) / 139.0f);
        if (step >= 500u && step < 640u) gz = 3.0f * sinf(2.0f * PI_F * (float)(step - 500u) / 139.0f);
        if (step >= 900u && step < 1040u) {
            float phase = (float)(step - 900u) / 139.0f;
            ax = 7.0f * sinf(4.0f * PI_F * phase);
            gz = 2.5f * sinf(6.0f * PI_F * phase);
        }
        if (step >= 1200u && step < 1340u) az += 1.8f * sinf(2.0f * PI_F * (float)(step - 1200u) / 139.0f);
        if (step >= 1500u && step < 1840u) { ax = 4.0f; gz = 2.0f; }
        if (step >= 100u && step < 340u) expected = 2;
        if (step >= 500u && step < 740u) expected = 5;
        feed(&imported, ax + noise, noise * 0.3f, az + noise * 0.5f, 0.0f, 0.0f, gz, 0);
        assert(imported.stream_count <= GE_MAX_SAMPLES - 32u && imported.event_count <= 4u);
        if (recognized != before) {
            if ((int)last_class == expected) ++positives;
            else ++false_accepts;
        }
    }
    printf("Live 20min: positives=%u false_accepts=%u rejected_bouts=%u previews=%u engine=%zu B CPU=%.3fs\n",
           positives, false_accepts, unknown, previews, sizeof(imported), (double)(clock() - started) / CLOCKS_PER_SEC);
    assert(positives == 240u && false_accepts == 0u && unknown == 360u);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length && !memcmp(snapshot, blob, length));
}

static void test_live_prehistory_and_full_capacity(void)
{
    unsigned i, t, p, k;
    static const unsigned durations[] = {450u, 700u, 1150u};
    for (i = 0u; i < 4u; ++i) {
        ge_reset_stream(&imported);
        clear_events();
        motion_samples(&imported, 2, 1800u, 1.0f, (int)i);
        assert(recognized == 0u);
        motion_samples(&imported, 0, 700u, 1.0f, (int)i);
        printf("Live continuous prehistory pose=%u matches=%u actions=%u\n", i, matched, recognized);
        assert(recognized == 1u && last_class == 2u);
    }
    /* Deliberately distant synthetic classes fill every slot to exercise the
     * actual worst-case 8 * 3 scan schedule, including three DTWs per feed.
     * These are scheduling fixtures, not a claimed eight-gesture dataset. */
    for (i = 0u; i < GE_MAX_CLASSES; ++i) {
        ge_class_t *c = &imported.model.classes[i];
        if (c->used) continue;
        *c = imported.model.classes[5];
        c->template_count = GE_TEMPLATES_PER_CLASS;
        for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t) {
            c->templates[t].duration_ms = 700u;
            for (p = 0u; p < GE_POINTS; ++p)
                for (k = 0u; k < GE_FEATURES; ++k)
                    c->templates[t].values[p][k] = (int16_t)(2000u + i * 400u + t * 100u);
        }
    }
    for (i = 0u; i < 3u; ++i) {
        ge_reset_stream(&imported);
        clear_events();
        motion_samples(&imported, 0, durations[i], 1.0f, 3);
        idle(&imported, 350u, 3);
        printf("Live eight-class schedule %ums: matches=%u actions=%u\n", durations[i], matched, recognized);
        assert(matched > 0u && recognized == 1u && last_class == 2u);
    }
    ge_reset_stream(&imported);
    clear_events();
    motion(&imported, 2, 900u, 1.0f, 0);
    assert(recognized == 0u && unknown == 1u && matched == 0u);
}

static void test_class_limit_preserves_hidden_slots(void)
{
    size_t length;
    ge_event_t event;
    ge_init(&imported);
    idle(&imported, 600u, 0);
    enroll(&imported, 7u, "HIDDEN", 0);
    length = ge_model_export(&imported, blob, sizeof(blob));
    assert(ge_set_class_limit(&imported, 0u) == GE_ERR_ARGUMENT);
    assert(ge_set_class_limit(&imported, 9u) == GE_ERR_ARGUMENT);
    assert(ge_set_class_limit(NULL, 1u) == GE_ERR_ARGUMENT);
    assert(imported.class_limit == 8u);
    assert(ge_set_class_limit(&imported, 1u) == GE_OK);
    assert(ge_class_count(&imported) == 1u && ge_active_class_count(&imported) == 0u);
    assert(ge_class_get(&imported, 7u));
    assert(ge_model_export(&imported, snapshot, sizeof(snapshot)) == length);
    assert(!memcmp(blob, snapshot, length));
    assert(ge_train_begin_at(&imported, 7u, "HIDDEN") == GE_ERR_ARGUMENT);
    idle(&imported, 600u, 0);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(recognized == 0u && matched == 0u);
    assert(ge_set_class_limit(&imported, 8u) == GE_OK);
    idle(&imported, 600u, 0);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(recognized == 1u && last_class == 7u);
    assert(ge_set_class_limit(&imported, 1u) == GE_OK);
    assert(ge_train_begin(&imported, "BUSY") == GE_OK);
    assert(ge_set_class_limit(&imported, 8u) == GE_ERR_BUSY);
    assert(imported.class_limit == 1u && imported.training);
    ge_train_cancel(&imported);
    clear_events();
    enroll(&imported, 0u, "SAME_VISIBLE", 0);
    assert(accepted == 3u && rejected == 0u && warned == 0u);
    assert(ge_class_count(&imported) == 2u && ge_active_class_count(&imported) == 1u);
    assert(ge_train_begin(&imported, "FULL") == GE_ERR_FULL);
    assert(ge_class_get(&imported, 7u));

    /* Streaming must obey the same mask, including candidates already queued. */
    assert(ge_model_import(&imported, blob, length) == GE_OK);
    ge_set_streaming_recognition(&imported, 1);
    assert(ge_set_class_limit(&imported, 1u) == GE_OK);
    clear_events();
    motion(&imported, 0, 750u, 1.0f, 0);
    assert(recognized == 0u && matched == 0u);
    assert(ge_set_class_limit(&imported, 8u) == GE_OK);
    clear_events();
    motion(&imported, 0, 750u, 1.0f, 0);
    assert(recognized == 1u && last_class == 7u && matched > 0u);
    imported.events[0].type = GE_EVENT_RECOGNIZED;
    imported.events[0].class_id = 7u;
    imported.event_read = 0u;
    imported.event_write = imported.event_count = 1u;
    assert(ge_set_class_limit(&imported, 1u) == GE_OK);
    assert(!ge_next_event(&imported, &event));
    puts("Class limits preserve stored slots and mask learning, warnings and both matchers.");
}

static void test_twenty_demonstrations_compact_model(void)
{
    static const float variants[] = { 0.65f, 0.85f, 1.0f, 1.15f };
    static const unsigned speeds[] = { 450u, 700u, 1150u };
    ge_template_t first_three[GE_TEMPLATES_PER_CLASS];
    uint8_t saved;
    unsigned i, j, support;
    float previous_threshold = 0.0f;
    size_t length;
    prepare_manual(&imported);
    for (i = 0u; i < GE_MAX_TRAINING_DEMOS; ++i) {
        float variation = i < GE_TEMPLATES_PER_CLASS ? 1.0f : variants[i % 4u];
        unsigned duration = i < GE_TEMPLATES_PER_CLASS ? 700u : 600u + 50u * (i % 5u);
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        variant_motion_samples(&imported, duration, variation);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        events(&imported);
        assert(ge_training_progress(&imported) == i + 1u);
        assert(imported.pending_count == (i < 3u ? i + 1u : 3u));
        assert(imported.pending_threshold >= previous_threshold && imported.pending_threshold <= 0.180f);
        previous_threshold = imported.pending_threshold;
        support = 0u;
        for (j = 0u; j < imported.pending_count; ++j) support += imported.pending_support[j];
        assert(support == i + 1u);
        if (i + 1u == GE_TEMPLATES_PER_CLASS) memcpy(first_three, imported.pending, sizeof(first_three));
        if (i == 8u) {
            ge_template_t before[GE_TEMPLATES_PER_CLASS];
            memcpy(before, imported.pending, sizeof(before));
            assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
            motion_samples(&imported, 2, 700u, 1.0f, 0);
            assert(ge_train_capture_end(&imported, timestamp) == GE_ERR_INCONSISTENT);
            events(&imported);
            assert(ge_training_progress(&imported) == i + 1u);
            assert(!memcmp(before, imported.pending, sizeof(before)));
        }
    }
    assert(accepted == GE_MAX_TRAINING_DEMOS && rejected == 1u && ready == 1u);
    assert(memcmp(first_three, imported.pending, sizeof(first_three)) != 0);
    assert(ge_train_capture_begin(&imported, timestamp) == GE_ERR_NOT_READY);
    assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == 2u);
    assert(imported.model.classes[2].template_count == GE_TEMPLATES_PER_CLASS);
    length = ge_model_export(&imported, snapshot, sizeof(snapshot));
    assert(length == 14128u && ge_model_import(&imported, snapshot, length) == GE_OK);
    assert(ge_model_export(&imported, blob, sizeof(blob)) == length && !memcmp(snapshot, blob, length));
    ge_set_streaming_recognition(&imported, 1);
    for (i = 0u; i < 4u; ++i) {
        for (j = 0u; j < 3u; ++j) {
            ge_reset_stream(&imported);
            idle(&imported, 300u, 0);
            clear_events();
            variant_motion_samples(&imported, speeds[j], variants[i]);
            idle(&imported, 400u, 0);
            printf("Twenty-demo compressed model variation=%u speed=%u: actions=%u distance=%.6f\n",
                   i, speeds[j], recognized, last_recognition_distance);
            assert(recognized == 1u && last_class == 2u);
        }
    }
    ge_reset_stream(&imported);
    idle(&imported, 300u, 0);
    clear_events();
    motion(&imported, 1, 700u, 1.0f, 0);
    motion(&imported, 2, 700u, 1.0f, 0);
    motion(&imported, 3, 700u, 1.0f, 0);
    motion(&imported, 0, 700u, 0.4f, 0);
    assert(recognized == 0u && matched == 0u && unknown == 4u);
    puts("Twenty demonstrations: bounded representatives, support accounting, variation/speed retention, unchanged GDT1 format.");
}

static void half_motion_samples(ge_engine_t *e, int kind, unsigned duration, int tail, int tilt)
{
    unsigned i, count = duration / 5u;
    unsigned first = tail ? count / 2u : 0u, last = tail ? count : count / 2u;
    for (i = first; i < last; ++i) {
        float wave = sinf(2.0f * PI_F * (float)i / (float)(count - 1u));
        feed(e, 0.0f, 0.0f, GRAVITY + (kind == 0 ? 4.5f * wave : 0.0f),
             0.0f, 0.0f, kind == 1 ? 3.0f * wave : 0.0f, tilt);
    }
}

static void test_live_nested_complete_gestures(void)
{
    static const unsigned durations[] = { 450u, 700u, 1150u };
    uint8_t saved;
    unsigned i, speed, before, t, p, k, offset;
    ge_init(&imported);
    ge_set_manual_training(&imported, 1);
    ge_set_streaming_recognition(&imported, 1);
    idle(&imported, 300u, 0);
    for (i = 0u; i < 2u; ++i) {
        assert(ge_train_begin_at(&imported, (uint8_t)i, i ? "NESTED B" : "OUTER A") == GE_OK);
        assert(ge_train_capture_begin(&imported, timestamp) == GE_OK);
        motion_samples(&imported, (int)i, 700u, 1.0f, 0);
        assert(ge_train_capture_end(&imported, timestamp) == GE_OK);
        assert(ge_train_confirm(&imported, &saved) == GE_OK && saved == i);
    }
    for (i = 0u; i < 4u; ++i) {
        for (speed = 0u; speed < 3u; ++speed) {
            ge_reset_stream(&imported);
            idle(&imported, 300u, (int)i);
            clear_events();
            half_motion_samples(&imported, 0, 700u, 0, (int)i);
            assert(recognized == 0u);
            motion_samples(&imported, 1, durations[speed], 1.0f, (int)i);
            half_motion_samples(&imported, 0, 700u, 1, (int)i);
            printf("Nested half A / B / half A pose=%u B=%ums: actions=%u class=%u\n",
                   i, durations[speed], recognized, last_class);
            assert(recognized == 1u && last_class == 1u);
            idle(&imported, 500u, (int)i);
            assert(recognized == 1u);
        }
        ge_reset_stream(&imported);
        idle(&imported, 300u, (int)i);
        clear_events();
        motion_samples(&imported, 0, 700u, 1.0f, (int)i);
        assert(recognized == 1u && last_class == 0u);
        motion_samples(&imported, 1, 700u, 1.0f, (int)i);
        assert(recognized == 2u && last_class == 1u);
        motion_samples(&imported, 0, 700u, 1.0f, (int)i);
        assert(recognized == 3u && last_class == 0u);
        before = recognized;
        idle(&imported, 300u, (int)i);
        assert(recognized == before); /* Shifted trailing windows cannot retrigger. */
    }
    /* Frozen scan endpoints and per-class boundaries also survive timestamp
     * rollover and the maximum competitor scan cost. */
    for (i = 2u; i < GE_MAX_CLASSES; ++i) {
        ge_class_t *c = &imported.model.classes[i];
        *c = imported.model.classes[1];
        c->template_count = GE_TEMPLATES_PER_CLASS;
        for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t) {
            c->templates[t].duration_ms = 700u;
            for (p = 0u; p < GE_POINTS; ++p)
                for (k = 0u; k < GE_FEATURES; ++k)
                    c->templates[t].values[p][k] = (int16_t)(2000u + i * 400u + t * 100u);
        }
    }
    for (i = 0u; i < 2u; ++i) {
        imported.model.classes[i].template_count = GE_TEMPLATES_PER_CLASS;
        for (t = 1u; t < GE_TEMPLATES_PER_CLASS; ++t)
            imported.model.classes[i].templates[t] = imported.model.classes[i].templates[0];
    }
    for (i = 0u; i < 4u; ++i) {
        for (speed = 0u; speed < 3u; ++speed) {
            for (offset = 0u; offset < 8u; ++offset) {
                timestamp = UINT32_MAX - 450u;
                ge_reset_stream(&imported);
                idle(&imported, 300u + offset * 5u, (int)i);
                clear_events();
                half_motion_samples(&imported, 0, 700u, 0, (int)i);
                motion_samples(&imported, 1, durations[speed], 1.0f, (int)i);
                half_motion_samples(&imported, 0, 700u, 1, (int)i);
                if (recognized != 1u || last_class != 1u)
                    printf("Eight-class nested failure pose=%u speed=%u phase=%u actions=%u class=%u\n",
                           i, durations[speed], offset, recognized, last_class);
                assert(recognized == 1u && last_class == 1u);
                idle(&imported, 500u, (int)i);
                assert(recognized == 1u);
            }
            ge_reset_stream(&imported);
            idle(&imported, 300u, (int)i);
            clear_events();
            half_motion_samples(&imported, 0, 700u, 0, (int)i);
            motion_samples(&imported, 1, durations[speed], 1.0f, (int)i);
            motion_samples(&imported, 2, 300u, 1.0f, (int)i);
            assert(recognized == 1u && last_class == 1u);
            idle(&imported, 500u, (int)i);
            assert(recognized == 1u);
        }
        for (offset = 0u; offset < 8u; ++offset) {
            ge_reset_stream(&imported);
            idle(&imported, 300u + offset * 5u, (int)i);
            clear_events();
            motion_samples(&imported, 0, 450u, 1.0f, (int)i);
            motion_samples(&imported, 0, 450u, 1.0f, (int)i);
            motion_samples(&imported, 2, 700u, 1.0f, (int)i);
            assert(recognized == 2u && last_class == 0u);
            idle(&imported, 500u, (int)i);
            assert(recognized == 2u);
        }
    }
    ge_reset_stream(&imported);
    idle(&imported, 300u, 3);
    clear_events();
    motion(&imported, 2, 900u, 1.0f, 3);
    assert(recognized == 0u && matched == 0u && unknown == 1u);
    /* A nearby competitor must not become a second action from the same
     * samples; both full verification passes enforce the ambiguity margin. */
    imported.model.classes[7] = imported.model.classes[0];
    for (t = 0u; t < GE_TEMPLATES_PER_CLASS; ++t)
        for (p = 0u; p < GE_POINTS; ++p)
            for (k = 0u; k < GE_FEATURES; ++k)
                imported.model.classes[7].templates[t].values[p][k] =
                    (int16_t)((float)imported.model.classes[7].templates[t].values[p][k] * 0.98f);
    ge_reset_stream(&imported);
    idle(&imported, 300u, 0);
    clear_events();
    motion(&imported, 0, 700u, 1.0f, 0);
    assert(recognized == 0u && matched == 0u && unknown == 1u);
    puts("Nested continuous recognition: complete inner B, partial A rejection, A/B/A, arbitrary poses, three speeds and eight-slot rollover.");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_training_and_speed();
    test_incremental_conflict_unknown();
    test_noise_continuous_and_short();
    test_disabled_and_reset();
    test_serialization();
    test_horizontal_rotation_gravity();
    test_small_motion_boundary();
    test_eight_hour_stream();
    test_manual_without_stationary_calibration();
    test_manual_press_release_boundaries();
    test_manual_rejections_and_reset();
    test_manual_training_then_automatic_recognition();
    test_manual_training_variation();
    test_manual_similarity_warning_and_ambiguity();
    test_live_matching();
    test_live_twenty_minute_stream();
    test_live_prehistory_and_full_capacity();
    test_class_limit_preserves_hidden_slots();
    test_twenty_demonstrations_compact_model();
    test_live_nested_complete_gestures();
    puts("All gesture engine tests passed.");
    return 0;
}
