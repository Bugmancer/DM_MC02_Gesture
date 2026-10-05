#include "gesture_app.h"
#include "gesture_usb.h"
#include "gesture_board.h"
#include "gesture_engine.h"
#include "gesture_config.h"
#include "display.h"
#include "main.h"
#include "tim.h"
#include <stdio.h>
#include <string.h>

static ge_engine_t engine;
static display_view_t view;
static uint8_t model_blob[GC_BLOB_MAX];
static gesture_config_t config, config_draft;
static uint8_t settings_open, settings_row, settings_slot;
static volatile uint32_t sample_ticks;
static uint32_t processed_ticks, ready_mask, samples, sample_drops, imu_errors;
static uint32_t unknown_count, recognized_count, max_feed_cycles, max_read_cycles;
static uint32_t last_view_ms, last_status_ms, last_key_ms, transient_until;
static uint8_t selected, armed, training, calibrated, stream_raw, delete_confirm;
static uint8_t consecutive_errors;
static uint8_t user_was_down, capture_key_ready, capture_held, capture_interrupted;
static uint32_t capture_release_since;
static uint32_t last_rgb = 0xffffffffu;
static uint32_t rgb_match_until;
static uint32_t rgb_accepted_until;
static uint8_t rgb_match_slot = GE_CLASS_NONE;

static void engine_events(uint32_t now);
static void update_rgb(uint32_t now);
static void set_armed(int enable);
static void settings_begin(void);
static void settings_key(BoardKey key);

static void show_match_rgb(uint8_t slot, uint32_t now, uint32_t hold_ms)
{
    if (slot >= GE_MAX_CLASSES) return;
    rgb_match_until = now + hold_ms;
    rgb_accepted_until = rgb_match_until;
    rgb_match_slot = slot;
    update_rgb(now);
}

static void report_firmware(void)
{
    gesture_usb_log(0, "FIRMWARE,gesture-20261006-r9,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING,RAW_KEY,AUTO_RECOGNITION,DEVICE_CONFIG,GUI_CONFIG,RGB_TIMING\r\n");
}

static void report_capture(void)
{
    if (training) gesture_usb_log(0, "CAPTURE,MODE,KEY\r\nCAPTURE,%s,%u\r\n",
        capture_held ? "BEGIN" : "WAIT", (unsigned)selected + 1u);
    else gesture_usb_log(0, "CAPTURE,MODE,KEY\r\n");
}

static void message(const char *text)
{
    (void)snprintf(view.message, sizeof(view.message), "%s", text);
}

static void reset_stream(void)
{
    processed_ticks = sample_ticks;
    rgb_match_slot = GE_CLASS_NONE;
    rgb_accepted_until = 0u;
    ge_reset_stream(&engine);
}

static void disarm(void)
{
    armed = 0u;
    ge_set_recognition(&engine, 0);
    reset_stream();
    gesture_usb_log(0, "STATE,IDLE\r\n");
}

static void cancel(void)
{
    if (capture_held) gesture_usb_log(0, "CAPTURE,END,%u\r\n", (unsigned)selected + 1u);
    capture_held = capture_interrupted = capture_key_ready = 0u;
    ge_train_cancel(&engine);
    training = delete_confirm = 0u;
    settings_open = 0u;
    transient_until = 0u;
    set_armed(1);
}

static void update_view(void)
{
    uint8_t i;
    view.selected_slot = selected;
    view.learning_count = ge_training_progress(&engine);
    view.learning_target = config.demo_target;
    view.settings_open = settings_open;
    view.settings_row = settings_row;
    view.class_limit = settings_open ? config_draft.class_limit : config.class_limit;
    view.config_demos = settings_open ? config_draft.demo_target : config.demo_target;
    view.config_rgb_hold_ms = settings_open ? config_draft.rgb_hold_ms : config.rgb_hold_ms;
    view.config_slot = settings_slot;
    memcpy(view.slot_colors, settings_open ? config_draft.colors : config.colors, sizeof(view.slot_colors));
    view.imu_ok = (uint8_t)((ready_mask & BOARD_READY_IMU) && !consecutive_errors);
    view.flash_ok = (uint8_t)((ready_mask & BOARD_READY_FLASH) != 0u);
    view.power_ok = 1u; /* Running from the system rail; no battery gauge is fitted here. */
    view.rejected_count = unknown_count;
    view.sample_drops = sample_drops;
    for (i = 0; i < GE_MAX_CLASSES; ++i) {
        const ge_class_t *c = ge_class_get(&engine, i);
        view.slot_templates[i] = c ? c->template_count : 0u;
    }
    display_set_view(&view);
}

static void startup_paint(uint32_t duration_ms)
{
    uint32_t start = HAL_GetTick();
    update_view();
    while ((uint32_t)(HAL_GetTick() - start) < duration_ms) display_process();
}

static void calibrate(void)
{
    if (!(ready_mask & BOARD_READY_IMU)) {
        message("IMU missing. Check board and reset");
        gesture_usb_log(0, "ERROR,CALIBRATE,IMU_UNAVAILABLE\r\n");
        return;
    }
    cancel();
    disarm();
    (void)HAL_TIM_Base_Stop_IT(&htim7);
    message("Lay flat: calibrating gyro");
    startup_paint(120u);
    calibrated = (uint8_t)board_calibrate_gyro(1500u);
    reset_stream();
    message(calibrated ? "Calibrated. Select an empty slot" : "Calibration failed; learning available");
    view.state = calibrated ? DISPLAY_STATE_IDLE : DISPLAY_STATE_ERROR;
    gesture_usb_log(0, "CALIBRATION,%s\r\n", calibrated ? "OK" : "FAILED");
    if (!calibrated) gesture_usb_log(0, "ERROR,CALIBRATE,NOT_STILL\r\n");
    if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) {
        ready_mask &= ~BOARD_READY_IMU;
        message("Sample timer failed");
        view.state = DISPLAY_STATE_ERROR;
        gesture_usb_log(0, "ERROR,CALIBRATE,TIMER_FAILED\r\n");
    } else set_armed(1);
}

static void set_armed(int enable)
{
    if (!enable) { cancel(); return; }
    if (!(ready_mask & BOARD_READY_IMU) || consecutive_errors >= 3u) {
        disarm();
        view.state = DISPLAY_STATE_ERROR;
        message("IMU missing. Check board and reset");
        gesture_usb_log(0, "ERROR,ARM,IMU_UNAVAILABLE\r\n");
        return;
    }
    if (training || delete_confirm || settings_open) {
        message("Finish or cancel learning first");
        gesture_usb_log(0, "ERROR,ARM,BUSY\r\n");
        return;
    }
    if (armed) return;
    reset_stream();
    armed = 1u;
    ge_set_recognition(&engine, 1);
    transient_until = 0u;
    view.state = DISPLAY_STATE_ARMED;
    message(ge_active_class_count(&engine) ? "Automatic recognition" : "No gestures. OK to learn");
    gesture_usb_log(0, "STATE,ARMED\r\n");
}

static void begin_learning(uint8_t slot)
{
    ge_status_t status;
    char name[GE_NAME_BYTES];
    if (training || delete_confirm || settings_open) {
        message("Finish or cancel current operation");
        gesture_usb_log(0, "ERROR,LEARN,BUSY\r\n");
        return;
    }
    if (!(ready_mask & BOARD_READY_IMU) || consecutive_errors >= 3u) {
        message("IMU unavailable. Check board and reset");
        gesture_usb_log(0, "ERROR,LEARN,IMU_UNAVAILABLE\r\n");
        return;
    }
    if (!(ready_mask & BOARD_READY_FLASH)) {
        message("Flash unavailable. Cannot save");
        gesture_usb_log(0, "ERROR,LEARN,FLASH_UNAVAILABLE\r\n");
        return;
    }
    if (slot >= config.class_limit) {
        message("Slot disabled in device settings");
        gesture_usb_log(0, "ERROR,LEARN,SLOT_DISABLED\r\n");
        return;
    }
    disarm();
    (void)snprintf(name, sizeof(name), "Gesture %u", (unsigned)slot + 1u);
    status = ge_train_begin_at(&engine, slot, name);
    if (status != GE_OK) {
        set_armed(1);
        message(ge_status_string(status));
        gesture_usb_log(0, "ERROR,LEARN,%s\r\n", ge_status_string(status));
        return;
    }
    selected = slot;
    training = 1u;
    capture_held = capture_interrupted = capture_key_ready = 0u;
    user_was_down = board_user_key_down();
    capture_release_since = HAL_GetTick();
    transient_until = 0u;
    view.state = DISPLAY_STATE_LEARNING;
    message(user_was_down ? "RELEASE KEY, then HOLD to record" : "HOLD KEY, move, RELEASE KEY");
    gesture_usb_log(0, "TRAIN,BEGIN,%u\r\n", (unsigned)slot + 1u);
    report_capture();
}

static void request_delete(uint8_t slot)
{
    if (training || settings_open) {
        message("Finish or cancel learning first");
        gesture_usb_log(0, "ERROR,DELETE,BUSY\r\n");
        return;
    }
    if (!ge_class_get(&engine, slot)) {
        message("No saved gesture at this slot");
        gesture_usb_log(0, "ERROR,DELETE,SLOT_EMPTY\r\n");
        return;
    }
    disarm();
    selected = slot;
    delete_confirm = 1u;
    transient_until = 0u;
    view.state = DISPLAY_STATE_CONFIRM;
    message("Delete slot? OK confirm, DOWN cancel");
    gesture_usb_log(0, "DELETE,CONFIRM,%u\r\n", (unsigned)slot + 1u);
}

static void save_model(void)
{
    size_t new_size;
    ge_class_t previous;
    ge_class_t *candidate;
    uint8_t id = selected;
    ge_status_t status = GE_OK;
    int deleting = delete_confirm != 0u;
    int saved = 0;
    const char *operation = deleting ? "DELETE" : "SAVE";
    if ((!training || ge_training_progress(&engine) < config.demo_target || capture_held) && !deleting) {
        message(capture_held ? "Release KEY before saving" : "Complete configured demonstrations");
        gesture_usb_log(0, "ERROR,SAVE,NOT_READY\r\n");
        return;
    }
    if (!(ready_mask & BOARD_READY_FLASH)) {
        message("Flash unavailable");
        gesture_usb_log(0, "ERROR,%s,FLASH_UNAVAILABLE\r\n", operation);
        return;
    }
    (void)HAL_TIM_Base_Stop_IT(&htim7);
    ge_reset_stream(&engine);
    view.state = DISPLAY_STATE_SAVING;
    message("Writing templates to Flash");
    startup_paint(80u);
    candidate = &engine.model.classes[selected];
    previous = *candidate;
    memset(candidate, 0, sizeof(*candidate));
    if (!deleting) {
        candidate->used = 1u;
        candidate->template_count = ge_training_progress(&engine);
        candidate->threshold = engine.pending_threshold;
        memcpy(candidate->name, engine.pending_name, sizeof(candidate->name));
        memcpy(candidate->templates, engine.pending, sizeof(candidate->templates));
    }
    new_size = gc_export(&config, &engine, model_blob, sizeof(model_blob));
    *candidate = previous;
    /* Persist a candidate image before committing the engine transaction, so a
     * failed write leaves every accepted demonstration available for retry. */
    if (new_size && board_store_save(model_blob, (uint32_t)new_size)) {
        status = deleting ? ge_class_delete(&engine, selected) : ge_train_confirm(&engine, &id);
        saved = status == GE_OK;
    }
    if (saved) {
        gesture_usb_log(0, "%s,%u,%lu\r\n", deleting ? "DELETED" : "SAVED",
                        (unsigned)id + 1u, (unsigned long)new_size);
        training = delete_confirm = 0u;
        capture_held = capture_interrupted = capture_key_ready = 0u;
    } else {
        message("Save failed. OK retries; DOWN cancels");
        gesture_usb_log(0, "ERROR,SAVE_ROLLED_BACK\r\n");
        gesture_usb_log(0, "ERROR,%s,ROLLED_BACK\r\n", operation);
        view.state = deleting ? DISPLAY_STATE_CONFIRM : DISPLAY_STATE_READY;
    }
    if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) {
        ready_mask &= ~BOARD_READY_IMU;
        disarm();
        message("Sample timer failed");
        view.state = DISPLAY_STATE_ERROR;
        gesture_usb_log(0, "ERROR,%s,TIMER_FAILED\r\n", operation);
    } else if (saved) {
        set_armed(1);
        message(deleting ? "Deleted. Automatic recognition" : "Saved. Automatic recognition");
    }
}

static void report_config(void)
{
    char lines[160];
    unsigned slot, offset;
    offset = (unsigned)snprintf(lines, sizeof(lines), "CONFIG,%u,%u\r\n",
        (unsigned)config.class_limit, (unsigned)config.demo_target);
    for (slot = 0u; slot < GE_MAX_CLASSES; ++slot)
        offset += (unsigned)snprintf(lines + offset, sizeof(lines) - offset,
            "COLOR,%u,%06lX\r\n", slot + 1u, (unsigned long)config.colors[slot]);
    gesture_usb_log(0, "%s", lines);
    gesture_usb_log(0, "TIMING,%u\r\n", (unsigned)config.rgb_hold_ms);
}

static void settings_begin(void)
{
    if (training || delete_confirm) {
        message("Finish or cancel learning first");
        gesture_usb_log(0, "ERROR,CONFIG,BUSY\r\n");
        return;
    }
    disarm();
    config_draft = config;
    settings_open = 1u;
    settings_row = 0u;
    settings_slot = selected < config.class_limit ? selected : 0u;
    transient_until = 0u;
    view.state = DISPLAY_STATE_IDLE;
    message("Device settings");
    update_view();
}

static int settings_save(void)
{
    size_t length;
    int saved;
    if (!(ready_mask & BOARD_READY_FLASH)) {
        message("Flash unavailable; settings not saved");
        gesture_usb_log(0, "ERROR,CONFIG,FLASH_UNAVAILABLE\r\n");
        return 0;
    }
    length = gc_export(&config_draft, &engine, model_blob, sizeof(model_blob));
    (void)HAL_TIM_Base_Stop_IT(&htim7);
    saved = length && board_store_save(model_blob, (uint32_t)length);
    reset_stream();
    if (saved) {
        config = config_draft;
        (void)ge_set_class_limit(&engine, config.class_limit);
        if (selected >= config.class_limit) selected = (uint8_t)(config.class_limit - 1u);
        settings_open = 0u;
        report_config();
    } else {
        message("Settings save failed. OK retries");
        gesture_usb_log(0, "ERROR,CONFIG,SAVE_FAILED\r\n");
    }
    if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) {
        ready_mask &= ~BOARD_READY_IMU;
        view.state = DISPLAY_STATE_ERROR;
        message("Sample timer failed");
        gesture_usb_log(0, "ERROR,CONFIG,TIMER_FAILED\r\n");
    } else if (saved) set_armed(1);
    update_view();
    return saved;
}

/* One short command carries a complete configuration. Do not partially
 * change channels or persist separate writes for each control in the GUI. */
static void configure_command(const char *line)
{
    gesture_config_t candidate;
    unsigned limit, demos, slot, digit, hold_ms;
    char colors[GE_MAX_CLASSES][7], extra;
    int parsed, saved, consumed = 0;
    parsed = sscanf(line, "configure %u %u %6s %6s %6s %6s %6s %6s %6s %6s%n",
        &limit, &demos, colors[0], colors[1], colors[2], colors[3],
        colors[4], colors[5], colors[6], colors[7], &consumed);
    if (parsed != 10 || limit < 1u || limit > GE_MAX_CLASSES ||
        demos < 1u || demos > GE_TEMPLATES_PER_CLASS) goto invalid;
    candidate = config;
    candidate.class_limit = (uint8_t)limit;
    candidate.demo_target = (uint8_t)demos;
    parsed = sscanf(line + consumed, " %u %c", &hold_ms, &extra);
    if (parsed == 1) {
        if (hold_ms < GC_RGB_HOLD_MIN_MS || hold_ms > GC_RGB_HOLD_MAX_MS ||
            hold_ms % GC_RGB_HOLD_STEP_MS) goto invalid;
        candidate.rgb_hold_ms = (uint16_t)hold_ms;
    } else if (strspn(line + consumed, " \t\r\n") != strlen(line + consumed)) goto invalid;
    for (slot = 0u; slot < GE_MAX_CLASSES; ++slot) {
        uint32_t color = 0u;
        if (strlen(colors[slot]) != 6u) goto invalid;
        for (digit = 0u; digit < 6u; ++digit) {
            unsigned char ch = (unsigned char)colors[slot][digit];
            unsigned value;
            if (ch >= '0' && ch <= '9') value = ch - '0';
            else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10u;
            else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10u;
            else goto invalid;
            color = (color << 4) | value;
        }
        candidate.colors[slot] = color;
    }
    if (training || delete_confirm || settings_open) {
        gesture_usb_log(0, "ERROR,CONFIG,BUSY\r\nCONFIG_RESULT,FAILED\r\n");
        return;
    }
    disarm();
    config_draft = candidate;
    settings_open = 1u;
    saved = settings_save();
    if (!saved) {
        settings_open = 0u;
        if (ready_mask & BOARD_READY_IMU) set_armed(1);
        update_view();
    }
    gesture_usb_log(0, "CONFIG_RESULT,%s\r\n", saved ? "SAVED" : "FAILED");
    return;
invalid:
    gesture_usb_log(0, "ERROR,CONFIG,INVALID\r\nCONFIG_RESULT,FAILED\r\n");
}

static void settings_key(BoardKey key)
{
    int direction = key == BOARD_KEY_LEFT ? -1 : 1;
    uint8_t *value = NULL;
    if (key == BOARD_KEY_UP) settings_row = (uint8_t)((settings_row + 8u) % 9u);
    else if (key == BOARD_KEY_DOWN) settings_row = (uint8_t)((settings_row + 1u) % 9u);
    else if (key == BOARD_KEY_OK && settings_row == 7u) { settings_save(); return; }
    else if (key == BOARD_KEY_OK && settings_row == 8u) { cancel(); update_view(); return; }
    else if (key == BOARD_KEY_LEFT || key == BOARD_KEY_RIGHT || key == BOARD_KEY_OK) {
        if (settings_row == 0u) value = &config_draft.class_limit;
        if (settings_row == 1u) value = &config_draft.demo_target;
        if (value) {
            unsigned maximum = settings_row ? GE_TEMPLATES_PER_CLASS : GE_MAX_CLASSES;
            if (direction > 0 && *value < maximum) ++*value;
            if (direction < 0 && *value > 1u) --*value;
            if (settings_slot >= config_draft.class_limit) settings_slot = (uint8_t)(config_draft.class_limit - 1u);
        } else if (settings_row == 2u) {
            int duration = (int)config_draft.rgb_hold_ms + direction * (int)GC_RGB_HOLD_STEP_MS;
            if (duration < (int)GC_RGB_HOLD_MIN_MS) duration = (int)GC_RGB_HOLD_MIN_MS;
            if (duration > (int)GC_RGB_HOLD_MAX_MS) duration = (int)GC_RGB_HOLD_MAX_MS;
            config_draft.rgb_hold_ms = (uint16_t)duration;
        } else if (settings_row == 3u) {
            settings_slot = (uint8_t)((settings_slot +
                (direction > 0 ? 1u : config_draft.class_limit - 1u)) % config_draft.class_limit);
        } else if (settings_row >= 4u && settings_row <= 6u) {
            unsigned shift = (6u - settings_row) * 8u;
            int channel = (int)((config_draft.colors[settings_slot] >> shift) & 255u);
            channel = key == BOARD_KEY_OK ? (channel + 1) % 256 : channel + direction * 17;
            if (channel < 0) channel = 0;
            if (channel > 255) channel = 255;
            config_draft.colors[settings_slot] = (config_draft.colors[settings_slot] & ~(255u << shift)) |
                ((uint32_t)channel << shift);
        }
    }
    update_view();
    update_rgb(HAL_GetTick());
}

static void key_event(BoardKey key)
{
    if (settings_open) { settings_key(key); return; }
    switch (key) {
    case BOARD_KEY_LEFT:
    case BOARD_KEY_RIGHT:
        if (!training && !delete_confirm) selected = (uint8_t)((selected +
            (key == BOARD_KEY_RIGHT ? 1u : config.class_limit - 1u)) % config.class_limit);
        break;
    case BOARD_KEY_UP: settings_begin(); break;
    case BOARD_KEY_USER: break; /* USER press/release is handled together below. */
    case BOARD_KEY_DOWN: cancel(); break;
    case BOARD_KEY_OK:
        if (delete_confirm || (training && ge_training_progress(&engine) >= config.demo_target)) save_model();
        else if (!training && ge_class_get(&engine, selected)) request_delete(selected);
        else if (!training) begin_learning(selected);
        break;
    default: break;
    }
}

static void user_key_process(uint32_t now)
{
    uint8_t down = board_user_key_down();
    uint8_t pressed = (uint8_t)(down && !user_was_down);
    uint8_t released = (uint8_t)(!down && user_was_down);
    ge_status_t status;
    user_was_down = down;
    if (!training) return;
    /* A released baseline longer than hardware debounce excludes a key that
       was already held (including a pending press) when learning began. */
    if (down) capture_release_since = now;
    else if ((uint32_t)(now - capture_release_since) >= 50u) capture_key_ready = 1u;
    if (pressed && capture_key_ready && ge_training_progress(&engine) < config.demo_target) {
        capture_key_ready = 0u;
        status = ge_train_capture_begin(&engine, now);
        if (status == GE_OK) {
            capture_held = 1u;
            capture_interrupted = 0u;
            view.state = DISPLAY_STATE_LEARNING;
            message("Recording. RELEASE KEY when done");
            gesture_usb_log(0, "CAPTURE,BEGIN,%u\r\n", (unsigned)selected + 1u);
        } else {
            message("Release KEY, then retry recording");
            gesture_usb_log(0, "TRAIN,REJECT,%s\r\n", ge_status_string(status));
            gesture_usb_log(0, "CAPTURE,WAIT,%u\r\n", (unsigned)selected + 1u);
        }
    }
    if (released && capture_held) {
        capture_held = 0u;
        gesture_usb_log(0, "CAPTURE,END,%u\r\n", (unsigned)selected + 1u);
        status = ge_train_capture_end(&engine, now);
        engine_events(now);
        if (status == GE_ERR_NOT_READY) {
            view.state = ge_training_progress(&engine) >= config.demo_target ? DISPLAY_STATE_READY : DISPLAY_STATE_LEARNING;
            message("Capture interrupted. HOLD KEY to retry");
            gesture_usb_log(0, "TRAIN,REJECT,%s\r\n",
                capture_interrupted ? "SAMPLE GAP" : ge_status_string(status));
        }
        capture_interrupted = 0u;
        if (training && status == GE_OK && ge_training_progress(&engine) >= config.demo_target)
            save_model();
        if (training && ge_training_progress(&engine) < config.demo_target)
            gesture_usb_log(0, "CAPTURE,WAIT,%u\r\n", (unsigned)selected + 1u);
    }
}

static void report_status(void)
{
    unsigned long mhz = (unsigned long)(SystemCoreClock / 1000000u);
    report_firmware();
    gesture_usb_log(0, "STATUS,%lu,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
        (unsigned long)HAL_GetTick(), armed, ge_active_class_count(&engine), calibrated,
        (unsigned long)samples, (unsigned long)sample_drops, (unsigned long)imu_errors,
        (unsigned long)unknown_count, (unsigned long)gesture_usb_drops(),
        (unsigned long)(max_feed_cycles / mhz), (unsigned long)(max_read_cycles / mhz));
}

static void report_inventory(void)
{
    unsigned slot, offset = 0u;
    char lines[160];
    report_firmware();
    gesture_usb_log(0, "INFO,1,%lu,%u,%u,%u,%u,%u,%u\r\n",
        (unsigned long)ready_mask, (unsigned)display_is_ok(), training,
        (unsigned)selected + 1u, (unsigned)ge_training_progress(&engine),
        (unsigned)(training && ge_training_progress(&engine) >= config.demo_target), delete_confirm);
    for (slot = 0u; slot < GE_MAX_CLASSES; ++slot) {
        const ge_class_t *c = ge_class_get(&engine, (uint8_t)slot);
        char name[GE_NAME_BYTES];
        unsigned i = 0u;
        if (c) {
            while (i < sizeof(name) - 1u && c->name[i]) {
                unsigned char ch = (unsigned char)c->name[i];
                name[i++] = (ch >= 32u && ch <= 126u && ch != ',') ? (char)ch : '_';
            }
        }
        name[i] = '\0';
        offset += (unsigned)snprintf(lines + offset, sizeof(lines) - offset,
            "SLOT,%u,%u,%s\r\n", slot + 1u, c ? (unsigned)c->template_count : 0u, name);
        /* Four CSV lines share a queue entry, leaving room for control events. */
        if ((slot + 1u) % 4u == 0u || slot + 1u == GE_MAX_CLASSES) {
            gesture_usb_log(0, "%s", lines);
            offset = 0u;
        }
    }
    report_capture();
    report_config();
}

static void command(const char *line)
{
    unsigned slot;
    char extra;
    if (!strcmp(line, "status")) report_status();
    else if (!strcmp(line, "list")) report_inventory();
    else if (!strcmp(line, "config")) report_config();
    else if (!strncmp(line, "configure ", 10u)) configure_command(line);
    else if (!strcmp(line, "arm")) set_armed(1);
    else if (!strcmp(line, "disarm") || !strcmp(line, "cancel")) cancel();
    else if (!strcmp(line, "save")) save_model();
    else if (!strcmp(line, "calibrate")) calibrate();
    else if (!strcmp(line, "stream 1")) { stream_raw = 1u; gesture_usb_log(0, "STREAM,1\r\n"); }
    else if (!strcmp(line, "stream 0")) { stream_raw = 0u; gesture_usb_log(0, "STREAM,0\r\n"); }
    else if (sscanf(line, "learn %u %c", &slot, &extra) == 1 && slot >= 1u && slot <= 8u) begin_learning((uint8_t)(slot - 1u));
    else if (sscanf(line, "delete %u %c", &slot, &extra) == 1 && slot >= 1u && slot <= 8u) request_delete((uint8_t)(slot - 1u));
    else gesture_usb_log(0, "ERROR,BAD_COMMAND\r\n");
}

static void engine_events(uint32_t now)
{
    ge_event_t event;
    uint8_t similar_slot = GE_CLASS_NONE;
    while (ge_next_event(&engine, &event)) {
        switch (event.type) {
        case GE_EVENT_MATCH:
            if (!armed) break;
            view.best_slot = event.class_id < GE_MAX_CLASSES ? (int8_t)event.class_id : -1;
            view.best_score = event.distance;
            view.second_score = event.second_distance;
            if (view.best_slot >= 0 || view.state != DISPLAY_STATE_UNKNOWN ||
                (int32_t)(transient_until - now) <= 0) {
                view.state = view.best_slot >= 0 ? DISPLAY_STATE_MATCH : DISPLAY_STATE_ARMED;
                transient_until = 0u;
                message(view.best_slot >= 0 ? "Matching gesture" : "Tracking motion");
            }
            /* Candidates update scores only. Sharing their 200 ms preview
             * with the accepted-action LED made rejected matches look accepted. */
            gesture_usb_log(0, "MATCH,%lu,%u,%lu,%lu,%lu\r\n", (unsigned long)now,
                view.best_slot >= 0 ? (unsigned)event.class_id + 1u : 0u,
                (unsigned long)(event.distance < 1000.0f ? event.distance * 1000000.0f : 999999999.0f),
                (unsigned long)(event.second_distance < 1000.0f ? event.second_distance * 1000000.0f : 999999999.0f),
                (unsigned long)event.duration_ms);
            break;
        case GE_EVENT_RECOGNIZED:
            if (!armed || event.class_id >= config.class_limit) break;
            ++recognized_count;
            view.best_slot = (int8_t)event.class_id;
            view.best_score = event.distance;
            view.second_score = event.second_distance;
            view.state = DISPLAY_STATE_MATCH;
            transient_until = now + config.rgb_hold_ms;
            message("Gesture accepted");
            show_match_rgb(event.class_id, now, config.rgb_hold_ms);
            gesture_usb_log(0, "EVENT,%lu,%u,%lu,%lu,%lu\r\n", (unsigned long)now,
                (unsigned)event.class_id + 1u, (unsigned long)(event.distance * 1000000.0f),
                (unsigned long)(event.second_distance < 1000.0f ? event.second_distance * 1000000.0f : 999999999.0f),
                (unsigned long)event.duration_ms);
            break;
        case GE_EVENT_UNKNOWN:
            if (!armed) break;
            ++unknown_count;
            view.best_slot = -1;
            view.best_score = event.distance;
            view.second_score = event.second_distance;
            view.state = DISPLAY_STATE_UNKNOWN;
            transient_until = now + 600u;
            if ((int32_t)(rgb_accepted_until - now) <= 0) rgb_match_slot = GE_CLASS_NONE;
            message(event.status == GE_OK ? "No confident match" : ge_status_string(event.status));
            update_rgb(now);
            gesture_usb_log(0, "UNKNOWN,%lu,%s\r\n", (unsigned long)now,
                            event.status == GE_OK ? "NO_MATCH" : ge_status_string(event.status));
            break;
        case GE_EVENT_DEMO_ACCEPTED:
            message(event.training_count < config.demo_target ? "Accepted. HOLD KEY for next demo" : "Target reached. Saving automatically");
            view.state = event.training_count < config.demo_target ? DISPLAY_STATE_LEARNING : DISPLAY_STATE_READY;
            gesture_usb_log(0, "TRAIN,DEMO,%u,%u\r\n", (unsigned)selected + 1u, event.training_count);
            if (event.training_count > 1u && event.training_count >= config.demo_target) {
                view.state = DISPLAY_STATE_READY;
                gesture_usb_log(0, "TRAIN,READY,%u,%u\r\n", (unsigned)selected + 1u, event.training_count);
            }
            break;
        case GE_EVENT_DEMO_REJECTED:
            view.state = ge_training_progress(&engine) >= config.demo_target ? DISPLAY_STATE_READY : DISPLAY_STATE_LEARNING;
            (void)snprintf(view.message, sizeof(view.message), "%s. HOLD KEY to retry", ge_status_string(event.status));
            gesture_usb_log(0, "TRAIN,REJECT,%s\r\n", ge_status_string(event.status));
            break;
        case GE_EVENT_TRAIN_READY:
            if (ge_training_progress(&engine) < config.demo_target) break;
            view.state = DISPLAY_STATE_READY;
            message("Target reached. Saving automatically");
            gesture_usb_log(0, "TRAIN,READY,%u,%u\r\n", (unsigned)selected + 1u,
                (unsigned)ge_training_progress(&engine));
            break;
        case GE_EVENT_DEMO_WARNING:
            if (event.status != GE_ERR_CONFLICT || event.class_id >= GE_MAX_CLASSES) break;
            similar_slot = event.class_id;
            gesture_usb_log(0, "TRAIN,WARN,SIMILAR,%u,%lu,%lu\r\n", (unsigned)event.class_id + 1u,
                (unsigned long)(event.distance * 1000000.0f),
                (unsigned long)(event.second_distance * 1000000.0f));
            break;
        default: break;
        }
    }
    /* The accepted and ready events belong to the same capture; retain its warning. */
    if (similar_slot < GE_MAX_CLASSES) {
        view.state = ge_training_progress(&engine) >= config.demo_target ? DISPLAY_STATE_READY : DISPLAY_STATE_LEARNING;
        (void)snprintf(view.message, sizeof(view.message), "Similar to slot %u. Recording kept",
            (unsigned)similar_slot + 1u);
    }
}

static void sample_once(uint32_t ticks)
{
    BoardImuSample raw;
    ge_sample_t sample;
    uint32_t delta = ticks - processed_ticks, cycles, elapsed;
    processed_ticks = ticks;
    if (delta > 1u) {
        sample_drops += delta - 1u;
        if (capture_held) capture_interrupted = 1u;
        ge_reset_stream(&engine);
    }
    cycles = DWT->CYCCNT;
    if (!board_read_imu(&raw)) {
        ++imu_errors;
        if (consecutive_errors < 255u) ++consecutive_errors;
        if (capture_held) capture_interrupted = 1u;
        ge_reset_stream(&engine);
        if (consecutive_errors == 3u) {
            cancel();
            calibrated = 0u;
            view.state = DISPLAY_STATE_ERROR;
            message("IMU read failed. Check board connection");
            gesture_usb_log(0, "ERROR,IMU_READ\r\n");
        }
        return;
    }
    elapsed = DWT->CYCCNT - cycles;
    if (elapsed > max_read_cycles) max_read_cycles = elapsed;
    consecutive_errors = 0u;
    if (!armed && !training && !delete_confirm && !settings_open) set_armed(1);
    ++samples;
    sample.timestamp_ms = raw.timestamp_ms;
    sample.ax = raw.accel[0]; sample.ay = raw.accel[1]; sample.az = raw.accel[2];
    sample.gx = raw.gyro[0]; sample.gy = raw.gyro[1]; sample.gz = raw.gyro[2];
    cycles = DWT->CYCCNT;
    ge_feed(&engine, &sample);
    elapsed = DWT->CYCCNT - cycles;
    if (elapsed > max_feed_cycles) max_feed_cycles = elapsed;
    engine_events(raw.timestamp_ms);
    if (stream_raw) gesture_usb_log(1, "RAW,%lu,%lu,%ld,%ld,%ld,%ld,%ld,%ld,%u\r\n",
        (unsigned long)samples, (unsigned long)raw.timestamp_ms,
        (long)(sample.ax * 1000.0f), (long)(sample.ay * 1000.0f), (long)(sample.az * 1000.0f),
        (long)(sample.gx * 1000.0f), (long)(sample.gy * 1000.0f), (long)(sample.gz * 1000.0f),
        (unsigned)(board_user_key_down() != 0u));
}

static void update_rgb(uint32_t now)
{
    uint32_t color;
    if (view.state == DISPLAY_STATE_ERROR) color = (now / 150u) % 2u ? 0u : 0x180000u;
    else if (settings_open) color = config_draft.colors[settings_slot];
    else if (training) {
        if (capture_held) color = config.colors[selected];
        else color = ge_training_progress(&engine) >= config.demo_target ? 0x180c00u : 0x000004u;
    } else if (armed && rgb_match_slot < GE_MAX_CLASSES && (int32_t)(rgb_match_until - now) > 0)
        color = config.colors[rgb_match_slot];
    else if (view.state == DISPLAY_STATE_UNKNOWN) {
        uint32_t elapsed = now - (transient_until - 600u);
        color = (elapsed < 150u || (elapsed >= 300u && elapsed < 450u)) ? 0x180000u : 0u;
    }
    else if (armed) color = 0x001010u;
    else color = 0x020202u;
    if (color != last_rgb) {
        board_rgb((uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
        last_rgb = color;
    }
}

void gesture_app_init(void)
{
    uint32_t length = 0u;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    ge_init(&engine);
    ge_set_manual_training(&engine, 1);
    ge_set_streaming_recognition(&engine, 1);
    ge_set_recognition(&engine, 0);
    gc_defaults(&config);
    settings_open = settings_row = settings_slot = 0u;
    gesture_usb_init(command);
    memset(&view, 0, sizeof(view));
    view.best_slot = -1;
    (void)display_init();
    message("Starting sensors and storage");
    startup_paint(100u);
    ready_mask = board_init();
    user_was_down = board_user_key_down();
    if ((ready_mask & BOARD_READY_FLASH) && board_store_load(model_blob, sizeof(model_blob), &length)) {
        if (gc_import(&config, &engine, model_blob, length) != GE_OK) message("Stored model invalid; empty model");
    }
    calibrated = 0u;
    if (ready_mask & BOARD_READY_IMU) {
        reset_stream();
        message("Ready. Calibration optional");
        if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) {
            ready_mask &= ~BOARD_READY_IMU;
            view.state = DISPLAY_STATE_ERROR;
            message("Sample timer failed");
            gesture_usb_log(0, "ERROR,STARTUP,TIMER_FAILED\r\n");
        } else set_armed(1);
    } else {
        view.state = DISPLAY_STATE_ERROR;
        message("BMI088 not found. Check hardware");
        gesture_usb_log(0, "ERROR,STARTUP,IMU_UNAVAILABLE\r\n");
    }
    update_view();
    report_firmware();
}

void gesture_app_tick_isr(void) { ++sample_ticks; }

void gesture_app_process(void)
{
    uint32_t now;
    uint32_t ticks = sample_ticks;
    if (ticks != processed_ticks && (ready_mask & BOARD_READY_IMU)) sample_once(ticks);
    now = HAL_GetTick();
    if ((uint32_t)(now - last_key_ms) >= 5u) {
        BoardKey key;
        last_key_ms = now;
        key = board_poll_key(now);
        user_key_process(now);
        key_event(key);
    }
    gesture_usb_process();
    if (transient_until && (int32_t)(HAL_GetTick() - transient_until) >= 0) {
        transient_until = 0u;
        view.state = armed ? DISPLAY_STATE_ARMED : DISPLAY_STATE_IDLE;
        message(armed ? "Recognition enabled" : "Ready");
    }
    if ((uint32_t)(now - last_view_ms) >= 100u) {
        last_view_ms = now;
        update_view();
    }
    update_rgb(now);
    if ((uint32_t)(now - last_status_ms) >= 1000u) {
        last_status_ms = now;
        report_status();
    }
    display_process();
}
