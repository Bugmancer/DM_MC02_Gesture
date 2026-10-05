#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../App/gesture_app.c"

MockDwt mock_dwt;
MockCoreDebug mock_core_debug;
uint32_t SystemCoreClock = 480000000U;
TIM_HandleTypeDef htim7;
static uint32_t mock_tick;
static uint8_t stored[GC_BLOB_MAX];
static uint32_t stored_size;
static unsigned store_calls, event_logs, calibration_calls, log_calls, max_log_length;
static int timer_running, store_success, calibration_success, imu_read_success;
static uint8_t mock_user_down;
static uint32_t mock_rgb, board_ready_result;
static HAL_StatusTypeDef timer_start_result;
static char last_log[200], log_history[16384];

uint32_t HAL_GetTick(void) { return mock_tick++; }
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *timer)
{ (void)timer; timer_running = 0; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *timer)
{ (void)timer; timer_running = timer_start_result == HAL_OK; return timer_start_result; }
uint32_t board_init(void) { return board_ready_result; }
int board_calibrate_gyro(uint32_t duration_ms)
{
    assert(!timer_running && !armed);
    ++calibration_calls; mock_tick += duration_ms; return calibration_success;
}
int board_read_imu(BoardImuSample *sample)
{
    if (!imu_read_success) return 0;
    memset(sample, 0, sizeof(*sample));
    sample->timestamp_ms = mock_tick; sample->accel[2] = 9.80665f; return 1;
}
BoardKey board_poll_key(uint32_t now_ms) { (void)now_ms; return BOARD_KEY_NONE; }
uint8_t board_user_key_down(void) { return mock_user_down; }
void board_rgb(uint8_t r, uint8_t g, uint8_t b)
{ mock_rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
int board_store_load(void *buffer, uint32_t capacity, uint32_t *length)
{
    assert(capacity >= stored_size); memcpy(buffer, stored, stored_size);
    *length = stored_size; return stored_size != 0U;
}
int board_store_save(const void *data, uint32_t length)
{
    assert(!timer_running && length <= sizeof(stored)); ++store_calls;
    if (!store_success) return 0;
    memcpy(stored, data, length); stored_size = length; return 1;
}
void gesture_usb_init(gesture_command_fn handler) { assert(handler != NULL); }
void gesture_usb_receive(const uint8_t *data, uint32_t length) { (void)data; (void)length; }
void gesture_usb_process(void) {}
void gesture_usb_log(int raw, const char *format, ...)
{
    va_list args; size_t length; (void)raw;
    va_start(args, format); (void)vsnprintf(last_log, sizeof(last_log), format, args); va_end(args);
    length = strlen(last_log);
    assert(length < 192U && strlen(log_history) + length < sizeof(log_history));
    strcat(log_history, last_log); ++log_calls;
    if (length > max_log_length) max_log_length = (unsigned)length;
    if (strncmp(last_log, "EVENT,", 6U) == 0) ++event_logs;
}
uint32_t gesture_usb_drops(void) { return 0U; }
int gesture_usb_connected(void) { return 1; }
uint8_t display_init(void) { return 1U; }
void display_set_view(const display_view_t *new_view) { assert(new_view != NULL); }
void display_process(void) {}
uint8_t display_is_ok(void) { return 1U; }

static void clear_logs(void)
{ last_log[0] = log_history[0] = '\0'; log_calls = max_log_length = 0U; }
static void reset_runtime(void)
{
    mock_tick = 0U;
    sample_ticks = processed_ticks = ready_mask = samples = sample_drops = imu_errors = 0U;
    unknown_count = recognized_count = max_feed_cycles = max_read_cycles = 0U;
    last_view_ms = last_status_ms = last_key_ms = transient_until = 0U;
    selected = armed = training = calibrated = stream_raw = delete_confirm = 0U;
    consecutive_errors = 0U;
    mock_user_down = user_was_down = capture_held = capture_interrupted = capture_key_ready = 0U;
    capture_release_since = 0U; mock_rgb = 0U; last_rgb = 0xffffffffU;
    rgb_match_until = rgb_accepted_until = 0U; rgb_match_slot = GE_CLASS_NONE;
    clear_logs(); gesture_app_init();
}
static void reset_app(void)
{
    stored_size = 0U; store_calls = event_logs = calibration_calls = 0U;
    board_ready_result = BOARD_READY_IMU | BOARD_READY_KEYS | BOARD_READY_FLASH;
    imu_read_success = store_success = calibration_success = 1;
    timer_running = 0; timer_start_result = HAL_OK; reset_runtime();
    assert(armed && engine.recognizing && timer_running && !calibrated);
    assert(calibration_calls == 0U && config.class_limit == 8U && config.demo_target == 1U);
    assert(strstr(log_history, "gesture-20261006-r9"));
    assert(strstr(log_history, "RAW_KEY,AUTO_RECOGNITION,DEVICE_CONFIG"));
    assert(config.rgb_hold_ms == 3000U && strstr(log_history, "RGB_TIMING"));
    assert(max_log_length < 192U); clear_logs();
}
static void fixture_class(uint8_t slot)
{
    ge_class_t *c = &engine.model.classes[slot]; unsigned i;
    memset(c, 0, sizeof(*c)); c->used = 1U; c->template_count = GE_TEMPLATES_PER_CLASS; c->threshold = 0.1f;
    (void)snprintf(c->name, sizeof(c->name), "Old %u", (unsigned)slot);
    for (i = 0U; i < GE_TEMPLATES_PER_CLASS; ++i) c->templates[i].duration_ms = 700U;
}
static void pending_fixture(void)
{
    unsigned i; assert(training && engine.training);
    engine.training_count = config.demo_target; engine.pending_ready = 1U; engine.pending_threshold = 0.1f;
    for (i = 0U; i < config.demo_target; ++i) engine.pending[i].duration_ms = 700U;
}
static void rgb_event(ge_event_type_t type, uint8_t slot, uint32_t now)
{
    ge_event_t event = {0}; event.type = type; event.class_id = slot;
    event.distance = 0.02f; event.second_distance = 0.4f; engine.events[0] = event;
    engine.event_read = 0U; engine.event_write = engine.event_count = 1U; engine_events(now);
}
static void feed_sample(float az)
{
    ge_sample_t sample = {0}; mock_tick += 5U; sample.timestamp_ms = mock_tick; sample.az = az;
    ge_feed(&engine, &sample); engine_events(mock_tick);
}
static void key_press(void)
{
    mock_user_down = 0U; mock_tick += 60U; user_key_process(mock_tick);
    mock_user_down = 1U; user_key_process(mock_tick);
    assert(capture_held && ge_training_capturing(&engine) && !armed);
}
static void demonstrate(unsigned milliseconds)
{
    unsigned i, count = milliseconds / 5U; key_press();
    for (i = 0U; i < count; ++i)
        feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * (float)i / (float)(count - 1U)));
    mock_user_down = 0U; user_key_process(mock_tick);
}
static void settings_select(uint8_t row)
{ while (settings_row != row) key_event(BOARD_KEY_DOWN); }

static void test_autonomous_start_cancel_and_gate(void)
{
    reset_app(); fixture_class(0U); rgb_event(GE_EVENT_RECOGNIZED, 0U, mock_tick);
    assert(event_logs == 1U); command("disarm"); assert(armed && engine.recognizing);
    command("learn 2"); assert(training && !armed && !engine.recognizing);
    rgb_event(GE_EVENT_RECOGNIZED, 0U, mock_tick); assert(event_logs == 1U);
    command("arm"); assert(training && !armed && strstr(last_log, "BUSY"));
    key_event(BOARD_KEY_DOWN); assert(!training && armed && engine.recognizing);
    command("learn 1"); assert(!training && armed && view.state == DISPLAY_STATE_ARMED);
    assert(strstr(last_log, "ERROR,LEARN,CONFLICT"));
}
static void test_one_demo_automatically_saves_and_recognizes(void)
{
    ge_engine_t restored; gesture_config_t restored_config;
    reset_app(); command("learn 1"); assert(!engine.initialized && training); demonstrate(700U);
    assert(!training && !capture_held && armed && timer_running);
    assert(store_calls == 1U && ge_class_count(&engine) == 1U);
    assert(ge_class_get(&engine, 0U)->template_count == 1U);
    assert(strstr(log_history, "TRAIN,DEMO,1,1\r\n") && strstr(log_history, "SAVED,1,"));
    assert(strstr(log_history, "TRAIN,REJECT,NOT READY") == NULL);
    ge_init(&restored); assert(gc_import(&restored_config, &restored, stored, stored_size) == GE_OK);
    assert(ge_class_get(&restored, 0U)->template_count == 1U && restored_config.demo_target == 1U);
    reset_runtime(); assert(armed && ge_class_count(&engine) == 1U && calibration_calls == 0U);
}
static void test_configured_three_demos_and_target_gate(void)
{
    unsigned demo; reset_app(); config.demo_target = 3U; command("learn 1");
    for (demo = 0U; demo < 3U; ++demo) {
        demonstrate(700U + 50U * demo);
        if (demo < 2U) {
            assert(training && !armed && ge_training_progress(&engine) == demo + 1U);
            assert(view.state == DISPLAY_STATE_LEARNING && store_calls == 0U);
            command("save"); assert(training && store_calls == 0U && strstr(last_log, "NOT_READY"));
        }
    }
    assert(!training && armed && store_calls == 1U && ge_class_get(&engine, 0U)->template_count == 3U);
    reset_runtime(); assert(armed && config.demo_target == 3U && ge_class_count(&engine) == 1U);
}
static void test_save_failure_preserves_demonstrations_for_retry(void)
{
    ge_class_t original; reset_app(); fixture_class(0U); original = *ge_class_get(&engine, 0U);
    stored_size = (uint32_t)gc_export(&config, &engine, stored, sizeof(stored));
    command("learn 2"); store_success = 0; demonstrate(700U);
    assert(training && !armed && timer_running && view.state == DISPLAY_STATE_READY);
    assert(ge_training_progress(&engine) == 1U && ge_training_ready(&engine));
    assert(ge_class_count(&engine) == 1U && !memcmp(&original, ge_class_get(&engine, 0U), sizeof(original)));
    assert(strstr(log_history, "ERROR,SAVE_ROLLED_BACK")); store_success = 1; key_event(BOARD_KEY_OK);
    assert(!training && armed && ge_class_count(&engine) == 2U && store_calls == 2U);
    assert(ge_class_get(&engine, 1U)->template_count == 1U);
}
static void test_delete_cancel_failure_retry_and_resume(void)
{
    reset_app(); fixture_class(0U); key_event(BOARD_KEY_OK); assert(delete_confirm && !armed);
    key_event(BOARD_KEY_DOWN); assert(!delete_confirm && armed && store_calls == 0U);
    command("delete 1"); store_success = 0; key_event(BOARD_KEY_OK);
    assert(delete_confirm && !armed && ge_class_count(&engine) == 1U);
    assert(view.state == DISPLAY_STATE_CONFIRM && timer_running);
    store_success = 1; engine.capturing = 1U; key_event(BOARD_KEY_OK);
    assert(!delete_confirm && armed && ge_class_count(&engine) == 0U && store_calls == 2U);
}
static void test_settings_edit_save_reboot_and_hidden_slots(void)
{
    unsigned i; ge_class_t hidden; reset_app(); fixture_class(0U); fixture_class(7U);
    hidden = *ge_class_get(&engine, 7U); key_event(BOARD_KEY_UP);
    assert(settings_open && !armed && view.settings_open);
    for (i = 0U; i < 4U; ++i) key_event(BOARD_KEY_LEFT);
    settings_select(1U); key_event(BOARD_KEY_RIGHT); key_event(BOARD_KEY_RIGHT);
    settings_select(2U); key_event(BOARD_KEY_RIGHT); key_event(BOARD_KEY_RIGHT);
    assert(config_draft.rgb_hold_ms == 3200U && view.config_rgb_hold_ms == 3200U);
    settings_select(3U); key_event(BOARD_KEY_RIGHT);
    settings_select(4U); key_event(BOARD_KEY_RIGHT); key_event(BOARD_KEY_RIGHT);
    settings_select(5U); key_event(BOARD_KEY_RIGHT);
    settings_select(6U); key_event(BOARD_KEY_RIGHT); key_event(BOARD_KEY_OK);
    assert(config_draft.colors[1U] == 0x222912U && mock_rgb == 0x222912U);
    assert(config.class_limit == 8U && config.demo_target == 1U);
    settings_select(7U); key_event(BOARD_KEY_OK);
    assert(!settings_open && armed && timer_running && store_calls == 1U);
    assert(config.class_limit == 4U && config.demo_target == 3U && config.colors[1U] == 0x222912U);
    assert(ge_class_count(&engine) == 2U && ge_active_class_count(&engine) == 1U);
    assert(!memcmp(&hidden, ge_class_get(&engine, 7U), sizeof(hidden)));
    reset_runtime(); assert(armed && config.class_limit == 4U && config.demo_target == 3U);
    assert(config.rgb_hold_ms == 3200U);
    assert(config.colors[1U] == 0x222912U && ge_class_count(&engine) == 2U);
    assert(!memcmp(&hidden, ge_class_get(&engine, 7U), sizeof(hidden)));
    selected = 3U; key_event(BOARD_KEY_RIGHT); assert(selected == 0U);
    key_event(BOARD_KEY_LEFT); assert(selected == 3U);
    command("learn 5"); assert(!training && armed && strstr(last_log, "SLOT_DISABLED"));
    command("status"); assert(strstr(last_log, ",1,1,0,"));
}
static void test_settings_cancel_bounds_key_and_failed_write(void)
{
    gesture_config_t before; unsigned i; reset_app(); before = config; key_event(BOARD_KEY_UP);
    for (i = 0U; i < 20U; ++i) key_event(BOARD_KEY_LEFT);
    assert(config_draft.class_limit == 1U); settings_select(1U);
    for (i = 0U; i < 8U; ++i) key_event(BOARD_KEY_LEFT);
    assert(config_draft.demo_target == 1U); settings_select(2U);
    for (i = 0U; i < 35U; ++i) key_event(BOARD_KEY_LEFT);
    assert(config_draft.rgb_hold_ms == GC_RGB_HOLD_MIN_MS);
    for (i = 0U; i < 310U; ++i) key_event(BOARD_KEY_RIGHT);
    assert(config_draft.rgb_hold_ms == GC_RGB_HOLD_MAX_MS);
    settings_select(4U);
    for (i = 0U; i < 20U; ++i) key_event(BOARD_KEY_RIGHT);
    assert((config_draft.colors[0] >> 16) == 255U);
    key_event(BOARD_KEY_OK); assert((config_draft.colors[0] >> 16) == 0U);
    mock_user_down = 1U; user_key_process(mock_tick); assert(!training && !capture_held && !armed);
    settings_select(7U); store_success = 0; key_event(BOARD_KEY_OK);
    assert(settings_open && !armed && timer_running && !memcmp(&config, &before, sizeof(config)));
    assert(strstr(last_log, "ERROR,CONFIG,SAVE_FAILED")); settings_select(8U); key_event(BOARD_KEY_OK);
    assert(!settings_open && armed && !memcmp(&config, &before, sizeof(config)));
    command("learn 1"); key_event(BOARD_KEY_UP);
    assert(training && !settings_open && !armed && strstr(last_log, "BUSY"));
}
static void test_rgb_three_seconds_and_confirmed_replacement(void)
{
    reset_app(); config.colors[0U] = 0x102030U; config.colors[1U] = 0x304050U;
    rgb_event(GE_EVENT_RECOGNIZED, 0U, 1000U);
    assert(mock_rgb == 0x102030U && rgb_accepted_until == 4000U);
    rgb_event(GE_EVENT_MATCH, 1U, 1100U); rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 1150U);
    rgb_event(GE_EVENT_UNKNOWN, GE_CLASS_NONE, 1200U); update_rgb(3999U); assert(mock_rgb == 0x102030U);
    rgb_event(GE_EVENT_RECOGNIZED, 1U, 3999U); assert(mock_rgb == 0x304050U && rgb_accepted_until == 6999U);
    update_rgb(6998U); assert(mock_rgb == 0x304050U);
    view.state = DISPLAY_STATE_ARMED; update_rgb(6999U); assert(mock_rgb == 0x001010U);
    rgb_event(GE_EVENT_RECOGNIZED, 0U, 0xfffffff0U); update_rgb(0x00000ba7U); assert(mock_rgb == 0x102030U);
    view.state = DISPLAY_STATE_ARMED; update_rgb(0x00000ba8U); assert(mock_rgb == 0x001010U);
}
static void test_rgb_preview_is_not_an_accepted_indicator(void)
{
    reset_app(); update_rgb(999U);
    rgb_event(GE_EVENT_MATCH, 1U, 1000U); update_rgb(1000U);
    assert(mock_rgb == 0x001010U && event_logs == 0U && rgb_match_slot == GE_CLASS_NONE);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 1040U); update_rgb(1199U); assert(mock_rgb == 0x001010U);
    rgb_event(GE_EVENT_UNKNOWN, GE_CLASS_NONE, 1300U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 1310U); assert(view.state == DISPLAY_STATE_UNKNOWN && mock_rgb == 0x180000U);
    update_rgb(1450U); assert(mock_rgb == 0U); command("learn 1");
    rgb_event(GE_EVENT_RECOGNIZED, 1U, 1500U); assert(event_logs == 0U && training);
}
static void test_rgb_configurable_duration_and_idempotent_arm(void)
{
    const uint16_t durations[] = {100U, 700U, 5000U, 30000U};
    unsigned i;
    for (i = 0U; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        uint32_t start = i % 2U ? 0xffffff00U : 1000U;
        uint32_t end = start + durations[i];
        reset_app(); config.rgb_hold_ms = durations[i]; config.colors[0] = 0x102030U;
        rgb_event(GE_EVENT_RECOGNIZED, 0U, start);
        assert(rgb_accepted_until == end && rgb_match_until == end && mock_rgb == 0x102030U);
        command("arm"); assert(rgb_accepted_until == end && rgb_match_slot == 0U);
        if (i == 2U) {
            mock_tick = start + 10U;
            sample_once(processed_ticks + 2U);
            assert(sample_drops == 1U && rgb_match_slot == 0U && rgb_accepted_until == end);
        }
        rgb_event(GE_EVENT_MATCH, 1U, start + 20U);
        rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, start + 30U);
        rgb_event(GE_EVENT_UNKNOWN, GE_CLASS_NONE, start + 40U);
        update_rgb(end - 1U); assert(mock_rgb == 0x102030U);
        view.state = DISPLAY_STATE_ARMED; update_rgb(end); assert(mock_rgb == 0x001010U);
        rgb_event(GE_EVENT_RECOGNIZED, 1U, end + 1U);
        assert(mock_rgb == config.colors[1] && rgb_accepted_until == end + 1U + durations[i]);
    }
}
static void test_key_requires_release_and_ignores_unheld_motion(void)
{
    unsigned i; reset_app(); mock_user_down = 1U; command("learn 1");
    for (i = 0U; i < 150U; ++i) feed_sample(9.80665f + 4.5f * sinf((float)i * 0.05f));
    user_key_process(mock_tick); assert(!capture_held && ge_training_progress(&engine) == 0U);
    mock_user_down = 0U; user_key_process(mock_tick); mock_tick += 20U;
    mock_user_down = 1U; user_key_process(mock_tick); assert(!capture_held);
    mock_user_down = 0U; user_key_process(mock_tick); demonstrate(700U);
    assert(!training && armed && store_calls == 1U);
}
static void test_capture_short_long_gap_and_cancel(void)
{
    unsigned i; reset_app(); command("learn 1"); demonstrate(100U);
    assert(training && ge_training_progress(&engine) == 0U && strstr(log_history, "TOO SHORT"));
    demonstrate(2500U); assert(training && ge_training_progress(&engine) == 0U && strstr(log_history, "TOO LONG"));
    key_press(); for (i = 0U; i < 40U; ++i) feed_sample(13.80665f);
    sample_once(processed_ticks + 3U); assert(capture_held && capture_interrupted);
    mock_user_down = 0U; user_key_process(mock_tick);
    assert(training && ge_training_progress(&engine) == 0U && strstr(log_history, "SAMPLE GAP"));
    key_press(); command("cancel"); assert(!training && !capture_held && armed && store_calls == 0U);
}
static void test_save_during_capture_cannot_commit_partial_demo(void)
{
    reset_app(); config.demo_target = 2U; command("learn 1"); demonstrate(700U); key_press(); command("save");
    assert(training && capture_held && store_calls == 0U && strstr(last_log, "NOT_READY"));
    mock_user_down = 0U; user_key_process(mock_tick); assert(training && ge_training_progress(&engine) == 1U);
    demonstrate(750U); assert(!training && armed && store_calls == 1U && ge_class_get(&engine, 0U)->template_count == 2U);
}
static void test_calibration_and_fault_recovery_restore_automatic_mode(void)
{
    unsigned i; reset_app(); command("calibrate");
    assert(armed && calibrated && timer_running && calibration_calls == 1U);
    calibration_success = 0; command("calibrate");
    assert(armed && !calibrated && timer_running && calibration_calls == 2U);
    imu_read_success = 0; for (i = 0U; i < 3U; ++i) sample_once(processed_ticks + 1U);
    assert(!armed && consecutive_errors == 3U && view.state == DISPLAY_STATE_ERROR);
    command("learn 1"); assert(!training && strstr(last_log, "IMU_UNAVAILABLE"));
    imu_read_success = 1; sample_once(processed_ticks + 1U);
    assert(armed && consecutive_errors == 0U && engine.recognizing);
}
static void test_timer_failures_and_missing_imu_do_not_arm(void)
{
    reset_app(); timer_start_result = HAL_ERROR; reset_runtime();
    assert(!armed && !(ready_mask & BOARD_READY_IMU) && view.state == DISPLAY_STATE_ERROR);
    command("arm"); assert(!armed && strstr(last_log, "IMU_UNAVAILABLE"));
    reset_app(); board_ready_result &= ~BOARD_READY_IMU; reset_runtime();
    assert(!armed && view.state == DISPLAY_STATE_ERROR);
    reset_app(); command("learn 1"); pending_fixture(); timer_start_result = HAL_ERROR; command("save");
    assert(!armed && view.state == DISPLAY_STATE_ERROR && !(ready_mask & BOARD_READY_IMU));
    reset_app(); key_event(BOARD_KEY_UP); settings_select(7U); timer_start_result = HAL_ERROR; key_event(BOARD_KEY_OK);
    assert(!armed && view.state == DISPLAY_STATE_ERROR && !(ready_mask & BOARD_READY_IMU));
}
static void test_inventory_settings_and_log_bounds(void)
{
    unsigned slot; char line[48]; reset_app(); fixture_class(0U); fixture_class(7U);
    memcpy(engine.model.classes[7U].name, "Last,\r\n\xff", 8U); engine.model.classes[7U].name[8U] = '\0'; command("list");
    assert(strstr(log_history, "SLOT,8,3,Last____\r\n") && strstr(log_history, "CONFIG,8,1\r\n"));
    assert(strstr(log_history, "TIMING,3000\r\n"));
    for (slot = 0U; slot < 8U; ++slot) {
        (void)snprintf(line, sizeof(line), "COLOR,%u,%06lX\r\n", slot + 1U, (unsigned long)config.colors[slot]);
        assert(strstr(log_history, line));
    }
    assert(max_log_length < 192U); command("learn 2"); command("list");
    assert(strstr(log_history, "CAPTURE,WAIT,2\r\n")); command("status"); assert(strstr(last_log, ",0,2,0,"));
}
static void test_key_timestamp_and_raw_physical_key(void)
{
    unsigned down, reported_key; unsigned long seq, timestamp; long ax, ay, az, gx, gy, gz; int consumed;
    reset_app(); command("stream 1");
    for (down = 0U; down < 3U; ++down) {
        mock_user_down = (uint8_t)(down == 1U); mock_tick += 5U; clear_logs(); sample_once(processed_ticks + 1U);
        assert(sscanf(last_log, "RAW,%lu,%lu,%ld,%ld,%ld,%ld,%ld,%ld,%u%n",
            &seq, &timestamp, &ax, &ay, &az, &gx, &gy, &gz, &reported_key, &consumed) == 9);
        assert(reported_key == mock_user_down && !strcmp(last_log + consumed, "\r\n"));
        assert(armed && !training && !capture_held);
    }
    command("learn 1"); mock_tick += 60U; mock_user_down = 0U; user_key_process(mock_tick);
    mock_tick += 5U; mock_user_down = 1U; ++sample_ticks; gesture_app_process(); assert(capture_held);
    mock_tick += 5U; mock_user_down = 0U; ++sample_ticks; gesture_app_process();
    assert(!capture_held && strstr(log_history, "TRAIN,REJECT,NOT READY") == NULL);
}
static void test_similar_demonstrations_do_not_block_auto_save(void)
{
    reset_app(); command("learn 1"); demonstrate(700U); command("learn 2"); demonstrate(700U);
    assert(!training && armed && ge_class_count(&engine) == 2U && store_calls == 2U);
    assert(strstr(log_history, "TRAIN,WARN,SIMILAR,1,") != NULL && strstr(log_history, "TRAIN,REJECT,") == NULL);
}

static void test_gui_config_is_atomic_and_preserves_templates(void)
{
    unsigned writes;
    reset_app(); fixture_class(7U);
    command("configure 2 3 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 5000");
    assert(config.class_limit == 2U && config.demo_target == 3U && config.colors[0] == 0x123456U);
    assert(config.colors[7] == 0x010203U && armed && !settings_open);
    assert(config.rgb_hold_ms == 5000U);
    assert(ge_class_get(&engine, 7U) != NULL && store_calls == 1U);
    assert(strstr(log_history, "CONFIG_RESULT,SAVED"));
    reset_runtime();
    assert(config.colors[0] == 0x123456U && config.demo_target == 3U);
    assert(config.rgb_hold_ms == 5000U);
    assert(ge_class_get(&engine, 7U) != NULL && armed);
    writes = store_calls;
    command("configure 0 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203");
    command("configure 2 1 12345x abcdef 000000 00ff00 ff0000 0000ff ffffff 010203");
    command("configure 2 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 99");
    command("configure 2 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 30001");
    command("configure 2 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 555");
    command("configure 2 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 abc");
    command("configure 2 1 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203 3000 extra");
    assert(store_calls == writes && config.demo_target == 3U);
    store_success = 0;
    command("configure 8 1 000000 000000 000000 000000 000000 000000 000000 000000");
    assert(config.class_limit == 2U && config.colors[0] == 0x123456U && armed && !settings_open);
    assert(strstr(log_history, "CONFIG_RESULT,FAILED"));
    store_success = 1;
    command("configure 2 3 123456 abcdef 000000 00ff00 ff0000 0000ff ffffff 010203");
    assert(config.rgb_hold_ms == 5000U && store_calls == writes + 2U);
    command("learn 1"); writes = store_calls;
    command("configure 8 1 000000 000000 000000 000000 000000 000000 000000 000000");
    assert(training && !armed && store_calls == writes && config.class_limit == 2U);
}

int main(void)
{
    test_autonomous_start_cancel_and_gate();
    test_one_demo_automatically_saves_and_recognizes();
    test_configured_three_demos_and_target_gate();
    test_save_failure_preserves_demonstrations_for_retry();
    test_delete_cancel_failure_retry_and_resume();
    test_settings_edit_save_reboot_and_hidden_slots();
    test_settings_cancel_bounds_key_and_failed_write();
    test_rgb_three_seconds_and_confirmed_replacement();
    test_rgb_preview_is_not_an_accepted_indicator();
    test_rgb_configurable_duration_and_idempotent_arm();
    test_key_requires_release_and_ignores_unheld_motion();
    test_capture_short_long_gap_and_cancel();
    test_save_during_capture_cannot_commit_partial_demo();
    test_calibration_and_fault_recovery_restore_automatic_mode();
    test_timer_failures_and_missing_imu_do_not_arm();
    test_inventory_settings_and_log_bounds();
    test_key_timestamp_and_raw_physical_key();
    test_similar_demonstrations_do_not_block_auto_save();
    test_gui_config_is_atomic_and_preserves_templates();
    puts("Gesture app tests passed (19 autonomous-device scenarios, real engine/config with mocked hardware).");
    return 0;
}
