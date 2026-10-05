#include "gesture_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ge_engine_t source, destination, engine_before;
static gesture_config_t config, loaded, config_before;
static uint8_t blob[GC_BLOB_MAX], corrupted[GC_BLOB_MAX];
static uint8_t legacy[GE_MODEL_BLOB_MAX], exported[GE_MODEL_BLOB_MAX];

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void repair_crc(uint8_t *buffer, size_t length)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0u; i < length; ++i) {
        crc ^= (i >= 12u && i < 16u) ? 0u : buffer[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    write_u32(buffer + 12u, ~crc);
}

static void fixture_class(ge_engine_t *engine, unsigned slot, const char *name)
{
    ge_class_t *gesture = &engine->model.classes[slot];
    memset(gesture, 0, sizeof(*gesture));
    gesture->used = 1u;
    gesture->template_count = 1u;
    gesture->threshold = 0.1f;
    strcpy(gesture->name, name);
    gesture->templates[0].duration_ms = 700u;
    gesture->templates[0].values[12][0] = (int16_t)(100u + slot);
    gesture->templates[0].values[15][1] = (int16_t)(-100 - (int)slot);
}

static void assert_failed_unchanged(const uint8_t *data, size_t length, ge_status_t expected)
{
    engine_before = destination;
    config_before = loaded;
    assert(gc_import(&loaded, &destination, data, length) == expected);
    assert(memcmp(&destination, &engine_before, sizeof(destination)) == 0);
    assert(memcmp(&loaded, &config_before, sizeof(loaded)) == 0);
}

static void test_defaults_and_limits(void)
{
    unsigned limit, demos, id;
    gc_defaults(NULL);
    gc_defaults(&config);
    assert(config.class_limit == 8u && config.demo_target == 1u);
    assert(config.rgb_hold_ms == 3000u);
    assert(config.colors[0] == 0x180000u && config.colors[7] == 0x181818u);
    assert(gc_validate(NULL) == GE_ERR_ARGUMENT);
    for (limit = 1u; limit <= GE_MAX_CLASSES; ++limit)
        for (demos = 1u; demos <= GE_TEMPLATES_PER_CLASS; ++demos) {
            config.class_limit = (uint8_t)limit;
            config.demo_target = (uint8_t)demos;
            assert(gc_validate(&config) == GE_OK);
        }
    config.class_limit = 0u;
    assert(gc_validate(&config) == GE_ERR_ARGUMENT);
    config.class_limit = 9u;
    assert(gc_validate(&config) == GE_ERR_ARGUMENT);
    gc_defaults(&config);
    config.demo_target = 0u;
    assert(gc_validate(&config) == GE_ERR_ARGUMENT);
    config.demo_target = 4u;
    assert(gc_validate(&config) == GE_ERR_ARGUMENT);
    for (id = 0u; id < GE_MAX_CLASSES; ++id) {
        gc_defaults(&config);
        config.colors[id] = 0x1000000u;
        assert(gc_validate(&config) == GE_ERR_ARGUMENT);
    }
    gc_defaults(&config);
    for (limit = GC_RGB_HOLD_MIN_MS; limit <= GC_RGB_HOLD_MAX_MS;
         limit += GC_RGB_HOLD_STEP_MS) {
        config.rgb_hold_ms = (uint16_t)limit;
        assert(gc_validate(&config) == GE_OK);
    }
    {
        static const uint16_t invalid[] = {0u, 99u, 101u, 29999u, 30001u, 65535u};
        for (id = 0u; id < sizeof(invalid) / sizeof(invalid[0]); ++id) {
            config.rgb_hold_ms = invalid[id];
            assert(gc_validate(&config) == GE_ERR_ARGUMENT);
        }
    }
}

static void test_roundtrip_preserves_hidden_models(void)
{
    unsigned id, demos, limit;
    size_t length, legacy_length;
    ge_init(&source);
    fixture_class(&source, 0u, "FIRST");
    fixture_class(&source, 7u, "EIGHTH");
    legacy_length = ge_model_export(&source, legacy, sizeof(legacy));
    assert(legacy_length > 0u);
    for (demos = 1u; demos <= GE_TEMPLATES_PER_CLASS; ++demos) {
        for (limit = 1u; limit <= GE_MAX_CLASSES; ++limit) {
            gc_defaults(&config);
            config.class_limit = (uint8_t)limit;
            config.demo_target = (uint8_t)demos;
            config.rgb_hold_ms = (uint16_t)(limit * demos * 100u);
            for (id = 0u; id < GE_MAX_CLASSES; ++id)
                config.colors[id] = (id * 31u << 16) | ((255u - id * 17u) << 8) | (id * 29u);
            config.colors[0] = 0u;
            config.colors[7] = 0xffffffu;
            length = gc_export(&config, &source, blob, sizeof(blob));
            assert(length == legacy_length + GC_HEADER_BYTES);
            assert(length <= GC_BLOB_MAX && !memcmp(blob, "GCF1", 4u));
            assert(blob[4] == 2u && blob[5] == 0u);
            assert(blob[18] == (uint8_t)config.rgb_hold_ms);
            assert(blob[19] == (uint8_t)(config.rgb_hold_ms >> 8));
            ge_init(&destination);
            gc_defaults(&loaded);
            assert(gc_import(&loaded, &destination, blob, length) == GE_OK);
            assert(!memcmp(&config, &loaded, sizeof(config)));
            assert(destination.class_limit == limit);
            assert(ge_class_count(&destination) == 2u);
            assert(ge_active_class_count(&destination) == (limit == 8u ? 2u : 1u));
            assert(ge_class_get(&destination, 7u));
            assert(ge_model_export(&destination, exported, sizeof(exported)) == legacy_length);
            assert(!memcmp(legacy, exported, legacy_length));
        }
    }
    assert(gc_export(&config, &source, blob, length - 1u) == 0u);
    assert(gc_export(&config, &source, blob, GC_HEADER_BYTES - 1u) == 0u);
    config.colors[2] = 0xffffffffu;
    assert(gc_export(&config, &source, blob, sizeof(blob)) == 0u);
    assert(gc_export(NULL, &source, blob, sizeof(blob)) == 0u);
    assert(gc_export(&config, NULL, blob, sizeof(blob)) == 0u);
}

static void test_hold_time_roundtrip_and_legacy_envelope(void)
{
    size_t length, model_length;
    unsigned i;
    static const uint16_t durations[] = {100u, 3000u, 30000u};
    model_length = ge_model_export(&source, legacy, sizeof(legacy));
    gc_defaults(&config);
    config.class_limit = 2u;
    config.demo_target = 3u;
    config.colors[0] = 0xabcdefu;
    config.colors[7] = 0x123456u;
    for (i = 0u; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        config.rgb_hold_ms = durations[i];
        length = gc_export(&config, &source, blob, sizeof(blob));
        assert(length == model_length + GC_HEADER_BYTES);
        assert(gc_import(&loaded, &destination, blob, length) == GE_OK);
        assert(!memcmp(&loaded, &config, sizeof(config)));
        assert(ge_model_export(&destination, exported, sizeof(exported)) == model_length);
        assert(!memcmp(exported, legacy, model_length));
    }
    memcpy(corrupted, blob, length);
    write_u16(corrupted + 4u, 1u);
    write_u16(corrupted + 18u, 0u);
    repair_crc(corrupted, length);
    assert(gc_import(&loaded, &destination, corrupted, length) == GE_OK);
    config.rgb_hold_ms = 3000u;
    assert(!memcmp(&loaded, &config, sizeof(config)));
    assert(ge_class_count(&destination) == 2u && ge_active_class_count(&destination) == 1u);
    assert(!strcmp(ge_class_get(&destination, 7u)->name, "EIGHTH"));
    assert(ge_model_export(&destination, exported, sizeof(exported)) == model_length);
    assert(!memcmp(exported, legacy, model_length));
    length = gc_export(&loaded, &destination, blob, sizeof(blob));
    assert(blob[4] == 2u && blob[18] == 0xb8u && blob[19] == 0x0bu);
    assert(gc_import(&loaded, &destination, blob, length) == GE_OK);
    write_u16(corrupted + 18u, 100u);
    repair_crc(corrupted, length);
    assert_failed_unchanged(corrupted, length, GE_ERR_FORMAT);
}

static void test_legacy_import_uses_defaults_without_migration_loss(void)
{
    size_t length = ge_model_export(&source, legacy, sizeof(legacy));
    gc_defaults(&loaded);
    loaded.class_limit = 2u;
    loaded.demo_target = 3u;
    loaded.rgb_hold_ms = 30000u;
    loaded.colors[7] = 0x123456u;
    assert(ge_set_class_limit(&destination, 2u) == GE_OK);
    assert(gc_import(&loaded, &destination, legacy, length) == GE_OK);
    gc_defaults(&config);
    assert(!memcmp(&loaded, &config, sizeof(config)));
    assert(destination.class_limit == 8u && ge_class_count(&destination) == 2u);
    assert(!strcmp(ge_class_get(&destination, 7u)->name, "EIGHTH"));
    assert(ge_model_export(&destination, exported, sizeof(exported)) == length);
    assert(!memcmp(exported, legacy, length));
    legacy[length - 1u] ^= 1u;
    assert_failed_unchanged(legacy, length, GE_ERR_CRC);
}

static void test_corruption_and_invalid_settings_are_atomic(void)
{
    size_t length;
    unsigned i;
    static const unsigned offsets[] = {16u, 17u, 18u, 23u};
    static const uint8_t values[] = {0u, 4u, 1u, 1u};
    gc_defaults(&config);
    config.class_limit = 3u;
    config.demo_target = 2u;
    length = gc_export(&config, &source, blob, sizeof(blob));
    assert(length > 0u);
    for (i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        memcpy(corrupted, blob, length);
        corrupted[offsets[i]] = values[i];
        repair_crc(corrupted, length);
        assert_failed_unchanged(corrupted, length, GE_ERR_FORMAT);
    }
    memcpy(corrupted, blob, length);
    corrupted[30] ^= 1u;
    assert_failed_unchanged(corrupted, length, GE_ERR_CRC);
    memcpy(corrupted, blob, length);
    corrupted[length - 1u] ^= 1u;
    repair_crc(corrupted, length);
    assert_failed_unchanged(corrupted, length, GE_ERR_CRC);
    memcpy(corrupted, blob, length);
    corrupted[4] = 3u;
    repair_crc(corrupted, length);
    assert_failed_unchanged(corrupted, length, GE_ERR_FORMAT);
    {
        static const uint16_t invalid[] = {0u, 99u, 101u, 29999u, 30001u, 65535u};
        for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            memcpy(corrupted, blob, length);
            write_u16(corrupted + 18u, invalid[i]);
            repair_crc(corrupted, length);
            assert_failed_unchanged(corrupted, length, GE_ERR_FORMAT);
        }
    }
    memcpy(corrupted, blob, length);
    write_u32(corrupted + GC_HEADER_BYTES - 4u, (uint32_t)length);
    repair_crc(corrupted, length);
    assert_failed_unchanged(corrupted, length, GE_ERR_FORMAT);
    assert_failed_unchanged(blob, length - 1u, GE_ERR_FORMAT);
    assert_failed_unchanged(blob, 0u, GE_ERR_FORMAT);
    assert_failed_unchanged(blob, 3u, GE_ERR_FORMAT);
    assert_failed_unchanged(blob, GC_BLOB_MAX + 1u, GE_ERR_FORMAT);
    assert_failed_unchanged(NULL, length, GE_ERR_ARGUMENT);
    assert(ge_train_begin_at(&destination, 1u, "PENDING") == GE_OK);
    assert_failed_unchanged(blob, length, GE_ERR_BUSY);
    ge_train_cancel(&destination);
    assert(gc_import(&loaded, &destination, blob, length) == GE_OK);
    assert(loaded.class_limit == 3u && loaded.demo_target == 2u);
}

int main(void)
{
    test_defaults_and_limits();
    test_roundtrip_preserves_hidden_models();
    test_hold_time_roundtrip_and_legacy_envelope();
    test_legacy_import_uses_defaults_without_migration_loss();
    test_corruption_and_invalid_settings_are_atomic();
    puts("All gesture configuration tests passed.");
    return 0;
}
