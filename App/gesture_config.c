#include "gesture_config.h"

#include <string.h>

#define GC_VERSION 2u
#define GC_MODEL_LENGTH_OFFSET (20u + 4u * GE_MAX_CLASSES)

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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

static uint32_t config_crc(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0u; i < length; ++i) {
        crc ^= (i >= 12u && i < 16u) ? 0u : data[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void gc_defaults(gesture_config_t *config)
{
    static const uint32_t colors[GE_MAX_CLASSES] = {
        0x180000u, 0x001800u, 0x000018u, 0x181800u,
        0x001818u, 0x180018u, 0x180800u, 0x181818u
    };
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->class_limit = GE_MAX_CLASSES;
    config->demo_target = 1u;
    config->rgb_hold_ms = 3000u;
    memcpy(config->colors, colors, sizeof(colors));
}

ge_status_t gc_validate(const gesture_config_t *config)
{
    uint8_t id;
    if (!config || config->class_limit == 0u || config->class_limit > GE_MAX_CLASSES ||
        config->demo_target == 0u || config->demo_target > GE_TEMPLATES_PER_CLASS ||
        config->rgb_hold_ms < GC_RGB_HOLD_MIN_MS ||
        config->rgb_hold_ms > GC_RGB_HOLD_MAX_MS ||
        config->rgb_hold_ms % GC_RGB_HOLD_STEP_MS != 0u)
        return GE_ERR_ARGUMENT;
    for (id = 0u; id < GE_MAX_CLASSES; ++id)
        if (config->colors[id] > 0xffffffu) return GE_ERR_ARGUMENT;
    return GE_OK;
}

size_t gc_export(const gesture_config_t *config, const ge_engine_t *engine,
                 uint8_t *buffer, size_t capacity)
{
    size_t model_length, length;
    uint8_t id;
    if (!engine || !buffer || gc_validate(config) != GE_OK || capacity < GC_HEADER_BYTES)
        return 0u;
    model_length = ge_model_export(engine, buffer + GC_HEADER_BYTES, capacity - GC_HEADER_BYTES);
    if (!model_length) return 0u;
    length = GC_HEADER_BYTES + model_length;
    memset(buffer, 0, GC_HEADER_BYTES);
    memcpy(buffer, "GCF1", 4u);
    write_u16(buffer + 4u, GC_VERSION);
    write_u16(buffer + 6u, GC_HEADER_BYTES);
    write_u32(buffer + 8u, (uint32_t)length);
    buffer[16] = config->class_limit;
    buffer[17] = config->demo_target;
    write_u16(buffer + 18u, config->rgb_hold_ms);
    for (id = 0u; id < GE_MAX_CLASSES; ++id)
        write_u32(buffer + 20u + 4u * id, config->colors[id]);
    write_u32(buffer + GC_MODEL_LENGTH_OFFSET, (uint32_t)model_length);
    write_u32(buffer + 12u, config_crc(buffer, length));
    return length;
}

ge_status_t gc_import(gesture_config_t *config, ge_engine_t *engine,
                      const uint8_t *buffer, size_t length)
{
    gesture_config_t candidate;
    ge_status_t status;
    const uint8_t *model;
    size_t model_length;
    uint16_t version;
    uint8_t id;
    if (!config || !engine || !buffer) return GE_ERR_ARGUMENT;
    if (length < 4u || length > GC_BLOB_MAX) return GE_ERR_FORMAT;
    gc_defaults(&candidate);
    if (memcmp(buffer, "GDT1", 4u) == 0) {
        model = buffer;
        model_length = length;
    } else {
        if (length < GC_HEADER_BYTES || memcmp(buffer, "GCF1", 4u) ||
            read_u16(buffer + 6u) != GC_HEADER_BYTES ||
            read_u32(buffer + 8u) != length) return GE_ERR_FORMAT;
        version = read_u16(buffer + 4u);
        if (version != 1u && version != GC_VERSION) return GE_ERR_FORMAT;
        if (read_u32(buffer + 12u) != config_crc(buffer, length)) return GE_ERR_CRC;
        if (version == 1u) {
            if (read_u16(buffer + 18u) != 0u) return GE_ERR_FORMAT;
        } else {
            candidate.rgb_hold_ms = read_u16(buffer + 18u);
        }
        candidate.class_limit = buffer[16];
        candidate.demo_target = buffer[17];
        for (id = 0u; id < GE_MAX_CLASSES; ++id)
            candidate.colors[id] = read_u32(buffer + 20u + 4u * id);
        if (gc_validate(&candidate) != GE_OK) return GE_ERR_FORMAT;
        model_length = read_u32(buffer + GC_MODEL_LENGTH_OFFSET);
        if (model_length != length - GC_HEADER_BYTES) return GE_ERR_FORMAT;
        model = buffer + GC_HEADER_BYTES;
    }
    /* ge_model_import validates before its commit. No fallible work remains
     * afterwards: validated limits are accepted once import has ruled out
     * an active training/capture transaction. */
    status = ge_model_import(engine, model, model_length);
    if (status != GE_OK) return status;
    (void)ge_set_class_limit(engine, candidate.class_limit);
    *config = candidate;
    return GE_OK;
}
