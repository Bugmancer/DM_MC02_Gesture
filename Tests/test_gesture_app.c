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
static uint8_t stored[GE_MODEL_BLOB_MAX];
static uint32_t stored_size;
static unsigned store_calls, event_logs, calibration_calls;
static unsigned log_calls, max_log_length;
static int timer_running, store_success, calibration_success;
static uint8_t mock_user_down;
static uint32_t mock_rgb;
static uint32_t board_ready_result;
static int imu_read_success;
static HAL_StatusTypeDef timer_start_result;
static char last_log[200];
static char log_history[8192];

uint32_t HAL_GetTick(void) { return mock_tick++; }
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *timer)
{
    (void)timer;
    timer_running = 0;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *timer)
{
    (void)timer;
    timer_running = timer_start_result == HAL_OK;
    return timer_start_result;
}
uint32_t board_init(void) { return board_ready_result; }
int board_calibrate_gyro(uint32_t duration_ms)
{
    assert(!timer_running);
    ++calibration_calls;
    mock_tick += duration_ms;
    return calibration_success;
}
int board_read_imu(BoardImuSample *sample)
{
    if (!imu_read_success) return 0;
    memset(sample, 0, sizeof(*sample));
    sample->timestamp_ms = mock_tick;
    sample->accel[2] = 9.80665f;
    return 1;
}
BoardKey board_poll_key(uint32_t now_ms) { (void)now_ms; return BOARD_KEY_NONE; }
uint8_t board_user_key_down(void) { return mock_user_down; }
void board_rgb(uint8_t r, uint8_t g, uint8_t b) { mock_rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
int board_store_load(void *buffer, uint32_t capacity, uint32_t *length)
{
    assert(capacity >= stored_size);
    memcpy(buffer, stored, stored_size);
    *length = stored_size;
    return stored_size != 0U;
}
int board_store_save(const void *data, uint32_t length)
{
    assert(!timer_running);
    assert(length <= sizeof(stored));
    ++store_calls;
    if (!store_success) return 0;
    memcpy(stored, data, length);
    stored_size = length;
    return 1;
}
void gesture_usb_init(gesture_command_fn handler) { assert(handler != NULL); }
void gesture_usb_receive(const uint8_t *data, uint32_t length) { (void)data; (void)length; }
void gesture_usb_process(void) {}
void gesture_usb_log(int raw, const char *format, ...)
{
    va_list args;
    size_t length;
    (void)raw;
    va_start(args, format);
    (void)vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    length = strlen(last_log);
    assert(strlen(log_history) + length < sizeof(log_history));
    strcat(log_history, last_log);
    ++log_calls;
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
{
    last_log[0] = log_history[0] = '\0';
    log_calls = max_log_length = 0U;
}

static void reset_app(void)
{
    mock_tick = 0U;
    stored_size = 0U;
    store_calls = event_logs = calibration_calls = 0U;
    board_ready_result = BOARD_READY_IMU | BOARD_READY_KEYS | BOARD_READY_FLASH;
    imu_read_success = 1;
    store_success = 1;
    calibration_success = 1;
    timer_running = 0;
    timer_start_result = HAL_OK;
    sample_ticks = processed_ticks = ready_mask = samples = sample_drops = imu_errors = 0U;
    unknown_count = recognized_count = max_feed_cycles = max_read_cycles = 0U;
    last_view_ms = last_status_ms = last_key_ms = transient_until = 0U;
    selected = armed = training = calibrated = stream_raw = delete_confirm = 0U;
    consecutive_errors = 0U;
    mock_user_down = user_was_down = capture_held = capture_interrupted = capture_key_ready = 0U;
    capture_release_since = 0U;
    mock_rgb = 0U;
    last_rgb = 0xffffffffU;
    rgb_match_until = 0U;
    rgb_match_slot = GE_CLASS_NONE;
    clear_logs();
    gesture_app_init();
    assert(!calibrated && timer_running && calibration_calls == 0U);
    assert(strncmp(log_history, "CALIBRATION,", 12U) != 0);
    assert(strstr(log_history, "\r\nCALIBRATION,") == NULL);
    assert(!armed && !engine.recognizing);
    assert(strstr(log_history, "FIRMWARE,gesture-20261005-r6,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING,RAW_KEY\r\n") != NULL);
    assert(max_log_length < 192U);
    clear_logs();
}

static void fixture_class(uint8_t slot)
{
    ge_class_t *c = &engine.model.classes[slot];
    unsigned i;
    memset(c, 0, sizeof(*c));
    c->used = 1U;
    c->template_count = GE_TEMPLATES_PER_CLASS;
    c->threshold = 0.1f;
    (void)snprintf(c->name, sizeof(c->name), "Old %u", (unsigned)slot);
    for (i = 0U; i < GE_TEMPLATES_PER_CLASS; ++i) c->templates[i].duration_ms = 700U;
}

static void pending_fixture(void)
{
    unsigned i;
    assert(training && engine.training);
    engine.training_count = GE_TEMPLATES_PER_CLASS;
    engine.pending_ready = 1U;
    engine.pending_threshold = 0.1f;
    for (i = 0U; i < GE_TEMPLATES_PER_CLASS; ++i) engine.pending[i].duration_ms = 700U;
}

static void inject_match(void)
{
    ge_event_t event = {0};
    event.type = GE_EVENT_RECOGNIZED;
    event.class_id = 0U;
    event.distance = 0.02f;
    event.second_distance = 0.9f;
    engine.events[0] = event;
    engine.event_read = 0U;
    engine.event_write = 1U;
    engine.event_count = 1U;
    engine_events(mock_tick);
}

static void test_arming_and_event_gate(void)
{
    reset_app();
    command("arm");
    assert(!armed);
    fixture_class(0U);
    ready_mask &= ~BOARD_READY_IMU;
    command("arm");
    assert(!armed);
    ready_mask |= BOARD_READY_IMU;
    command("arm");
    assert(armed && engine.recognizing);
    inject_match();
    assert(event_logs == 1U);
    key_event(BOARD_KEY_DOWN);
    assert(!armed && !engine.recognizing);
    inject_match();
    assert(event_logs == 1U);
    command("learn 2");
    assert(training && !armed);
    command("arm");
    assert(!armed && training);
}

static void feed_sample(float az, uint32_t *timestamp)
{
    ge_sample_t sample = {0};
    *timestamp += 5U;
    sample.timestamp_ms = *timestamp;
    sample.az = az;
    ge_feed(&engine, &sample);
    engine_events(*timestamp);
}

static void test_learning_requires_explicit_confirmation(void)
{
    unsigned demo, i;
    uint32_t timestamp;
    ge_engine_t restored;
    reset_app();
    timestamp = mock_tick;
    command("learn 1");
    assert(training && selected == 0U);
    key_event(BOARD_KEY_RIGHT);
    assert(selected == 0U);
    command("save");
    assert(training && ge_class_count(&engine) == 0U && store_calls == 0U);
    for (i = 0U; i < 140U; ++i) feed_sample(9.80665f, &timestamp);
    for (demo = 0U; demo < 3U; ++demo) {
        user_key_process(timestamp);
        mock_user_down = 1U;
        user_key_process(timestamp);
        assert(capture_held && ge_training_capturing(&engine) && !armed);
        for (i = 0U; i < 140U; ++i) {
            float phase = (float)i / 139.0f;
            feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * phase), &timestamp);
        }
        assert(ge_training_progress(&engine) == demo);
        mock_user_down = 0U;
        user_key_process(timestamp);
        for (i = 0U; i < 180U; ++i) feed_sample(9.80665f, &timestamp);
    }
    assert(ge_training_ready(&engine));
    assert(view.state == DISPLAY_STATE_READY);
    assert(ge_class_count(&engine) == 0U && store_calls == 0U);
    key_event(BOARD_KEY_OK);
    assert(!training && !armed && ge_class_count(&engine) == 1U);
    assert(store_calls == 1U && timer_running);
    assert(view.state == DISPLAY_STATE_IDLE);
    ge_init(&restored);
    assert(ge_model_import(&restored, stored, stored_size) == GE_OK);
    assert(ge_class_count(&restored) == 1U);
}

static void test_save_failure_restores_old_model(void)
{
    uint8_t before[GE_MODEL_BLOB_MAX], after[GE_MODEL_BLOB_MAX];
    size_t size;
    reset_app();
    fixture_class(0U);
    size = ge_model_export(&engine, before, sizeof(before));
    memcpy(stored, before, size);
    stored_size = (uint32_t)size;
    command("learn 2");
    pending_fixture();
    store_success = 0;
    command("save");
    assert(view.state == DISPLAY_STATE_ERROR);
    assert(!training && !armed && !delete_confirm && timer_running);
    assert(ge_model_export(&engine, after, sizeof(after)) == size);
    assert(memcmp(before, after, size) == 0);
    assert(memcmp(before, stored, size) == 0);
    assert(ge_class_count(&engine) == 1U);
}

static void test_delete_cancel_and_failed_write(void)
{
    reset_app();
    fixture_class(0U);
    command("arm");
    key_event(BOARD_KEY_OK);
    assert(delete_confirm && !armed && view.state == DISPLAY_STATE_CONFIRM);
    key_event(BOARD_KEY_RIGHT);
    assert(selected == 0U);
    key_event(BOARD_KEY_DOWN);
    assert(!delete_confirm && ge_class_count(&engine) == 1U && store_calls == 0U);
    command("delete 1");
    store_success = 0;
    key_event(BOARD_KEY_OK);
    assert(view.state == DISPLAY_STATE_ERROR);
    assert(!delete_confirm && ge_class_count(&engine) == 1U);
}

static void test_delete_ignores_new_capture_during_confirmation(void)
{
    reset_app();
    fixture_class(0U);
    command("delete 1");
    /* A hand movement while the confirmation is open must not prevent deletion. */
    engine.capturing = 1U;
    key_event(BOARD_KEY_OK);
    assert(ge_class_count(&engine) == 0U);
    assert(store_calls == 1U && !delete_confirm && timer_running);
}

static void test_failed_learn_cannot_keep_armed_display(void)
{
    reset_app();
    fixture_class(0U);
    command("arm");
    assert(armed && view.state == DISPLAY_STATE_ARMED);
    command("learn 1");
    assert(!armed && !training);
    assert(view.state != DISPLAY_STATE_ARMED && view.state != DISPLAY_STATE_MATCH);
}

static void test_timer_restart_failure_blocks_arm(void)
{
    reset_app();
    command("learn 1");
    pending_fixture();
    timer_start_result = HAL_ERROR;
    command("save");
    assert(view.state == DISPLAY_STATE_ERROR);
    assert((ready_mask & BOARD_READY_IMU) == 0U);
    command("arm");
    assert(!armed);
}

static void test_inventory_restores_slots_and_pending_operation(void)
{
    unsigned slot, lines = 0U;
    const char *p;
    char expected[48];
    reset_app();
    fixture_class(0U);
    fixture_class(7U);
    memcpy(engine.model.classes[7].name, "Last,\r\n\xff", 8U);
    engine.model.classes[7].name[8] = '\0';
    command("status");
    assert(log_calls == 2U && strncmp(last_log, "STATUS,", 7U) == 0);
    assert(strncmp(log_history, "FIRMWARE,gesture-20261005-r6,", 27U) == 0);
    clear_logs();
    command("list");
    assert(log_calls == 5U && max_log_length < 192U);
    (void)snprintf(expected, sizeof(expected), "INFO,1,%lu,1,0,1,0,0,0\r\n", (unsigned long)ready_mask);
    assert(strstr(log_history, expected) != NULL);
    assert(strstr(log_history, "SLOT,1,3,Old 0\r\n") != NULL);
    assert(strstr(log_history, "SLOT,8,3,Last____\r\n") != NULL);
    for (slot = 2U; slot <= 7U; ++slot) {
        (void)snprintf(expected, sizeof(expected), "SLOT,%u,0,\r\n", slot);
        assert(strstr(log_history, expected) != NULL);
    }
    for (p = log_history; *p; ++p) if (*p == '\n') ++lines;
    assert(lines == 11U);
    assert(strstr(log_history, "CAPTURE,MODE,KEY\r\n") != NULL);
    command("learn 2");
    pending_fixture();
    clear_logs();
    command("list");
    (void)snprintf(expected, sizeof(expected), "INFO,1,%lu,1,1,2,3,1,0\r\n", (unsigned long)ready_mask);
    assert(strstr(log_history, expected) != NULL);
    assert(strstr(log_history, "SLOT,2,0,\r\n") != NULL);
    assert(strstr(log_history, "CAPTURE,WAIT,2\r\n") != NULL);
    command("cancel");
    assert(!strcmp(last_log, "STATE,IDLE\r\n"));
    command("delete 8");
    clear_logs();
    command("list");
    (void)snprintf(expected, sizeof(expected), "INFO,1,%lu,1,0,8,0,0,1\r\n", (unsigned long)ready_mask);
    assert(strstr(log_history, expected) != NULL);
}

static void test_commands_report_reasons_without_changing_pending_work(void)
{
    reset_app();
    command("arm");
    assert(!strcmp(last_log, "ERROR,ARM,NO_GESTURES\r\n"));
    command("delete 1");
    assert(!strcmp(last_log, "ERROR,DELETE,SLOT_EMPTY\r\n"));
    command("save");
    assert(!strcmp(last_log, "ERROR,SAVE,NOT_READY\r\n"));
    ready_mask &= ~BOARD_READY_IMU;
    command("calibrate");
    assert(!strcmp(last_log, "ERROR,CALIBRATE,IMU_UNAVAILABLE\r\n"));
    command("arm");
    assert(!strcmp(last_log, "ERROR,ARM,IMU_UNAVAILABLE\r\n"));
    command("learn 1");
    assert(!strcmp(last_log, "ERROR,LEARN,IMU_UNAVAILABLE\r\n"));
    ready_mask |= BOARD_READY_IMU;
    calibrated = 0U;
    command("learn 1");
    assert(training && !calibrated && calibration_calls == 0U);
    command("cancel");
    ready_mask &= ~BOARD_READY_FLASH;
    command("learn 1");
    assert(!strcmp(last_log, "ERROR,LEARN,FLASH_UNAVAILABLE\r\n"));
    ready_mask |= BOARD_READY_FLASH;
    command("learn 1");
    assert(strstr(log_history, "STATE,IDLE\r\nTRAIN,BEGIN,1\r\n") != NULL);
    command("arm");
    assert(!strcmp(last_log, "ERROR,ARM,BUSY\r\n"));
    command("learn 2");
    assert(!strcmp(last_log, "ERROR,LEARN,BUSY\r\n"));
    command("delete 1");
    assert(!strcmp(last_log, "ERROR,DELETE,BUSY\r\n"));
    assert(training && selected == 0U);
    pending_fixture();
    ready_mask &= ~BOARD_READY_FLASH;
    command("save");
    assert(!strcmp(last_log, "ERROR,SAVE,FLASH_UNAVAILABLE\r\n"));
    assert(training && ge_training_ready(&engine));
    ready_mask |= BOARD_READY_FLASH;
    store_success = 0;
    command("save");
    assert(strstr(log_history, "ERROR,SAVE,ROLLED_BACK\r\nSTATE,IDLE\r\n") != NULL);
}

static void test_calibration_and_delete_failure_reasons(void)
{
    reset_app();
    calibration_success = 0;
    command("calibrate");
    assert(strstr(log_history, "CALIBRATION,FAILED\r\nERROR,CALIBRATE,NOT_STILL\r\n") != NULL);
    assert(!calibrated && timer_running && calibration_calls == 1U);
    fixture_class(0U);
    command("arm");
    assert(armed && !calibrated && calibration_calls == 1U);
    command("learn 2");
    assert(training && !armed && !calibrated && calibration_calls == 1U);
    command("cancel");
    calibration_success = 1;
    command("calibrate");
    assert(calibration_calls == 2U);
    assert(strstr(log_history, "CALIBRATION,OK\r\n") != NULL);
    assert(calibrated && !armed);
    command("delete 1");
    ready_mask &= ~BOARD_READY_FLASH;
    command("save");
    assert(!strcmp(last_log, "ERROR,DELETE,FLASH_UNAVAILABLE\r\n"));
    assert(delete_confirm);
    ready_mask |= BOARD_READY_FLASH;
    store_success = 0;
    command("save");
    assert(strstr(log_history, "ERROR,DELETE,ROLLED_BACK\r\nSTATE,IDLE\r\n") != NULL);
    assert(ge_class_count(&engine) == 1U);
    timer_start_result = HAL_ERROR;
    command("calibrate");
    assert(!strcmp(last_log, "ERROR,CALIBRATE,TIMER_FAILED\r\n"));
    assert(!(ready_mask & BOARD_READY_IMU));
}

static void test_key_capture_requires_release_then_fresh_press(void)
{
    uint32_t timestamp;
    unsigned i;
    reset_app();
    mock_user_down = 1U;
    command("learn 1");
    timestamp = mock_tick;
    for (i = 0U; i < 200U; ++i) {
        feed_sample(9.80665f, &timestamp);
        user_key_process(timestamp);
    }
    assert(training && !armed && !capture_held && !ge_training_capturing(&engine));
    assert(strstr(view.message, "RELEASE KEY") != NULL);
    mock_user_down = 0U;
    user_key_process(timestamp);
    for (i = 0U; i < 9U; ++i) {
        feed_sample(9.80665f, &timestamp);
        user_key_process(timestamp);
    }
    assert(!capture_key_ready);
    feed_sample(9.80665f, &timestamp);
    user_key_process(timestamp);
    assert(capture_key_ready);
    mock_user_down = 1U;
    user_key_process(timestamp);
    key_event(BOARD_KEY_USER);
    assert(capture_held && ge_training_capturing(&engine) && !armed);
    assert(strstr(view.message, "RELEASE KEY") != NULL);
    clear_logs();
    command("list");
    assert(strstr(log_history, "CAPTURE,BEGIN,1\r\n") != NULL);
    for (i = 0U; i < 150U; ++i) feed_sample(9.80665f, &timestamp);
    assert(capture_held && ge_training_capturing(&engine));
    assert(ge_training_progress(&engine) == 0U);
    mock_user_down = 0U;
    user_key_process(timestamp);
    assert(!capture_held && !ge_training_capturing(&engine) && !armed);
    assert(ge_training_progress(&engine) == 0U);
    assert(strstr(log_history, "CAPTURE,END,1\r\n") != NULL);
    assert(strstr(log_history, "TRAIN,REJECT,") != NULL);
    assert(strstr(log_history, "CAPTURE,WAIT,1\r\n") != NULL);
    pending_fixture();
    mock_user_down = 1U;
    user_key_process(timestamp + 100U);
    assert(training && !armed && !capture_held);
}

static void test_interrupted_and_cancelled_capture_cannot_save_fragments(void)
{
    uint32_t timestamp;
    unsigned i;
    reset_app();
    command("learn 1");
    timestamp = mock_tick;
    for (i = 0U; i < 140U; ++i) feed_sample(9.80665f, &timestamp);
    user_key_process(timestamp);
    mock_user_down = 1U;
    user_key_process(timestamp);
    assert(capture_held);
    for (i = 0U; i < 140U; ++i) {
        float phase = (float)i / 139.0f;
        feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * phase), &timestamp);
    }
    mock_tick = timestamp;
    sample_once(processed_ticks + 3U);
    assert(capture_held && capture_interrupted);
    mock_user_down = 0U;
    user_key_process(timestamp);
    assert(ge_training_progress(&engine) == 0U);
    assert(strstr(log_history, "TRAIN,REJECT,SAMPLE GAP\r\n") != NULL);
    for (i = 0U; i < 140U; ++i) feed_sample(9.80665f, &timestamp);
    user_key_process(timestamp);
    mock_user_down = 1U;
    user_key_process(timestamp);
    assert(capture_held);
    command("cancel");
    assert(!training && !capture_held && !ge_training_capturing(&engine));
    mock_user_down = 0U;
    user_key_process(timestamp);
    assert(!armed && ge_class_count(&engine) == 0U && store_calls == 0U);
}

static void test_user_only_records_learning_and_slot_colors(void)
{
    static const uint32_t expected[8] = {
        0x180000U, 0x001800U, 0x000018U, 0x181800U,
        0x001818U, 0x180018U, 0x180800U, 0x181818U
    };
    unsigned slot;
    reset_app();
    fixture_class(0U);
    mock_user_down = 1U;
    user_key_process(mock_tick);
    assert(!armed);
    key_event(BOARD_KEY_UP);
    assert(armed);
    user_key_process(mock_tick + 50U);
    assert(armed);
    mock_user_down = 0U;
    user_key_process(mock_tick + 100U);
    mock_user_down = 1U;
    user_key_process(mock_tick + 150U);
    assert(armed);
    key_event(BOARD_KEY_UP);
    assert(!armed);
    command("arm");
    for (slot = 0U; slot < 8U; ++slot) {
        view.state = DISPLAY_STATE_MATCH;
        view.best_slot = (int8_t)slot;
        show_match_rgb((uint8_t)slot, 100U, 1200U);
        update_rgb(100U);
        assert(mock_rgb == expected[slot]);
        update_rgb(300U);
        assert(mock_rgb == expected[slot]);
    }
    command("disarm");
    view.state = DISPLAY_STATE_UNKNOWN;
    transient_until = 1600U;
    update_rgb(1000U);
    assert(mock_rgb == 0x180000U);
    update_rgb(1150U);
    assert(mock_rgb == 0U);
    update_rgb(1300U);
    assert(mock_rgb == 0x180000U);
    update_rgb(1450U);
    assert(mock_rgb == 0U);
    view.state = DISPLAY_STATE_ERROR;
    update_rgb(600U);
    assert(mock_rgb == 0x180000U);
    update_rgb(750U);
    assert(mock_rgb == 0U);
    view.state = DISPLAY_STATE_LEARNING;
    training = 1U;
    capture_held = 0U;
    update_rgb(900U);
    assert(mock_rgb == 0x000004U);
    capture_held = 1U;
    selected = 6U;
    update_rgb(1000U);
    assert(mock_rgb == expected[6]);
}

static void test_live_match_changes_rgb_without_action_event(void)
{
    ge_event_t event = {0};
    reset_app();
    fixture_class(0U);
    fixture_class(1U);
    command("arm");
    clear_logs();
    event.type = GE_EVENT_MATCH;
    event.class_id = 1U;
    event.distance = 0.03f;
    event.second_distance = 0.4f;
    event.duration_ms = 450U;
    engine.events[0] = event;
    engine.event_read = 0U;
    engine.event_write = 1U;
    engine.event_count = 1U;
    engine_events(900U);
    update_rgb(900U);
    assert(event_logs == 0U && recognized_count == 0U);
    assert(view.state == DISPLAY_STATE_MATCH && view.best_slot == 1);
    assert(mock_rgb == 0x001800U);
    assert(strstr(log_history, "MATCH,900,2,") != NULL);
    event.class_id = GE_CLASS_NONE;
    engine.events[0] = event;
    engine.event_read = 0U;
    engine.event_write = 1U;
    engine.event_count = 1U;
    engine_events(940U);
    update_rgb(940U);
    assert(event_logs == 0U && view.best_slot == -1);
    assert(view.state == DISPLAY_STATE_ARMED && mock_rgb == 0x001800U);
    assert(strstr(log_history, "MATCH,940,0,") != NULL);
    update_rgb(1099U);
    assert(mock_rgb == 0x001800U);
    mock_tick = 1100U;
    last_view_ms = 1090U;
    gesture_app_process();
    assert(last_view_ms == 1090U && mock_rgb == 0x001010U);
    command("disarm");
    clear_logs();
    event.class_id = 0U;
    engine.events[0] = event;
    engine.event_read = 0U;
    engine.event_write = 1U;
    engine.event_count = 1U;
    engine_events(1100U);
    assert(log_calls == 0U && view.state == DISPLAY_STATE_IDLE);
}

static void rgb_event(ge_event_type_t type, uint8_t slot, uint32_t now)
{
    ge_event_t event = {0};
    event.type = type;
    event.class_id = slot;
    event.distance = 0.02f;
    event.second_distance = 0.4f;
    engine.events[0] = event;
    engine.event_read = 0U;
    engine.event_write = 1U;
    engine.event_count = 1U;
    engine_events(now);
}

static void test_rgb_accepted_hold_and_immediate_class_switch(void)
{
    reset_app();
    fixture_class(0U);
    fixture_class(1U);
    command("arm");
    rgb_event(GE_EVENT_RECOGNIZED, 0U, 2000U);
    assert(mock_rgb == 0x180000U && event_logs == 1U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 2040U);
    rgb_event(GE_EVENT_MATCH, 0U, 2100U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 2140U);
    update_rgb(3199U);
    assert(mock_rgb == 0x180000U);
    update_rgb(3200U);
    assert(mock_rgb == 0x001010U);
    rgb_event(GE_EVENT_RECOGNIZED, 0U, 4000U);
    rgb_event(GE_EVENT_MATCH, 1U, 4040U);
    assert(mock_rgb == 0x001800U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 4080U);
    update_rgb(4239U);
    assert(mock_rgb == 0x001800U);
    update_rgb(4240U);
    assert(mock_rgb == 0x001010U);
    rgb_event(GE_EVENT_MATCH, 0U, 0xfffffff0U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 0x18U);
    update_rgb(0xb7U);
    assert(mock_rgb == 0x180000U);
    update_rgb(0xb8U);
    assert(mock_rgb == 0x001010U);
}

static void test_unknown_indication_survives_empty_preview(void)
{
    ge_event_t event = {0};
    reset_app();
    fixture_class(1U);
    command("arm");
    rgb_event(GE_EVENT_RECOGNIZED, 1U, 1000U);
    assert(mock_rgb == 0x001800U);
    event.type = GE_EVENT_UNKNOWN;
    event.class_id = GE_CLASS_NONE;
    engine.events[0] = event;
    event.type = GE_EVENT_MATCH;
    engine.events[1] = event;
    engine.event_read = 0U;
    engine.event_write = 2U;
    engine.event_count = 2U;
    engine_events(1100U);
    assert(view.state == DISPLAY_STATE_UNKNOWN && transient_until == 1700U);
    assert(rgb_match_slot == GE_CLASS_NONE && mock_rgb == 0x180000U);
    rgb_event(GE_EVENT_MATCH, GE_CLASS_NONE, 1250U);
    update_rgb(1250U);
    assert(view.state == DISPLAY_STATE_UNKNOWN && mock_rgb == 0U);
    update_rgb(1400U);
    assert(mock_rgb == 0x180000U);
    mock_tick = 1700U;
    gesture_app_process();
    assert(view.state == DISPLAY_STATE_ARMED && mock_rgb == 0x001010U);
    rgb_event(GE_EVENT_UNKNOWN, GE_CLASS_NONE, 2000U);
    assert(view.state == DISPLAY_STATE_UNKNOWN && mock_rgb == 0x180000U);
    rgb_event(GE_EVENT_MATCH, 1U, 2040U);
    assert(view.state == DISPLAY_STATE_MATCH && mock_rgb == 0x001800U);
}

static void test_one_demo_can_save_or_add_without_interrupted_save(void)
{
    unsigned i;
    uint32_t timestamp;
    ge_engine_t restored;
    reset_app();
    command("learn 1");
    timestamp = mock_tick + 60U;
    user_key_process(timestamp);
    mock_user_down = 1U;
    user_key_process(timestamp);
    assert(capture_held && !engine.initialized);
    for (i = 0U; i < 140U; ++i) {
        float phase = (float)i / 139.0f;
        feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * phase), &timestamp);
    }
    mock_user_down = 0U;
    user_key_process(timestamp);
    assert(ge_training_progress(&engine) == 1U && ge_training_ready(&engine));
    assert(strstr(log_history, "TRAIN,READY,1,1\r\n") != NULL);
    assert(store_calls == 0U && ge_class_count(&engine) == 0U);
    timestamp += 60U;
    user_key_process(timestamp);
    mock_user_down = 1U;
    user_key_process(timestamp);
    assert(capture_held);
    command("save");
    assert(training && capture_held && store_calls == 0U);
    mock_user_down = 0U;
    user_key_process(timestamp + 10U);
    assert(ge_training_progress(&engine) == 1U && ge_training_ready(&engine));
    assert(view.state == DISPLAY_STATE_READY);
    user_key_process(timestamp + 70U);
    mock_user_down = 1U;
    user_key_process(timestamp + 70U);
    assert(capture_held);
    mock_tick = timestamp + 75U;
    sample_once(processed_ticks + 3U);
    mock_user_down = 0U;
    user_key_process(timestamp + 80U);
    assert(!capture_held && capture_interrupted == 0U);
    assert(ge_training_ready(&engine) && view.state == DISPLAY_STATE_READY);
    assert(strstr(log_history, "TRAIN,REJECT,SAMPLE GAP\r\n") != NULL);
    command("save");
    assert(!training && store_calls == 1U && ge_class_count(&engine) == 1U);
    assert(ge_class_get(&engine, 0U)->template_count == 1U);
    ge_init(&restored);
    assert(ge_model_import(&restored, stored, stored_size) == GE_OK);
    assert(ge_class_get(&restored, 0U)->template_count == 1U);
}

static void test_learning_ignores_unheld_motion_and_rejects_long_hold(void)
{
    unsigned i;
    uint32_t timestamp;
    reset_app();
    command("learn 1");
    timestamp = mock_tick;
    for (i = 0U; i < 140U; ++i) feed_sample(9.80665f, &timestamp);
    for (i = 0U; i < 140U; ++i) {
        float phase = (float)i / 139.0f;
        feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * phase), &timestamp);
    }
    for (i = 0U; i < 180U; ++i) feed_sample(9.80665f, &timestamp);
    assert(ge_training_progress(&engine) == 0U && !capture_held);
    user_key_process(timestamp);
    mock_user_down = 1U;
    user_key_process(timestamp);
    for (i = 0U; i < 600U; ++i)
        feed_sample(9.80665f + 4.5f * sinf((float)i * 0.05f), &timestamp);
    assert(capture_held && ge_training_capturing(&engine));
    assert(ge_training_progress(&engine) == 0U);
    mock_user_down = 0U;
    user_key_process(timestamp);
    assert(!capture_held && ge_training_progress(&engine) == 0U);
    assert(strstr(log_history, "TRAIN,REJECT,TOO LONG\r\n") != NULL);
}

static void test_similar_demo_can_save_without_losing_existing_action(void)
{
    unsigned slot, i;
    uint32_t timestamp;
    ge_class_t original;
    ge_engine_t restored;
    reset_app();
    timestamp = mock_tick;
    for (slot = 0U; slot < 2U; ++slot) {
        command(slot ? "learn 2" : "learn 1");
        clear_logs();
        timestamp += 60U;
        user_key_process(timestamp);
        mock_user_down = 1U;
        user_key_process(timestamp);
        assert(capture_held);
        for (i = 0U; i < 140U; ++i) {
            float phase = (float)i / 139.0f;
            feed_sample(9.80665f + 4.5f * sinf(6.28318530718f * phase), &timestamp);
        }
        mock_user_down = 0U;
        user_key_process(timestamp);
        assert(ge_training_progress(&engine) == 1U && ge_training_ready(&engine));
        assert(view.state == DISPLAY_STATE_READY && !delete_confirm);
        assert(strstr(log_history, "TRAIN,REJECT,") == NULL);
        if (slot) {
            unsigned similar;
            unsigned long distance, limit;
            const char *warning = strstr(log_history, "TRAIN,WARN,SIMILAR,");
            assert(warning != NULL);
            assert(sscanf(warning, "TRAIN,WARN,SIMILAR,%u,%lu,%lu", &similar, &distance, &limit) == 3);
            assert(similar == 1U && limit > 0U && distance < limit);
            assert(strstr(log_history, "TRAIN,DEMO,2,1\r\n") != NULL);
            assert(strstr(log_history, "TRAIN,READY,2,1\r\n") != NULL);
            assert(strstr(view.message, "Similar to slot 1") != NULL);
            assert(strstr(view.message, "OK still saves") != NULL);
            assert(memcmp(&original, ge_class_get(&engine, 0U), sizeof(original)) == 0);
        }
        command("save");
        assert(!training && !delete_confirm && ge_class_count(&engine) == slot + 1U);
        assert(store_calls == slot + 1U);
        assert(ge_class_get(&engine, (uint8_t)slot)->template_count == 1U);
        if (!slot) original = *ge_class_get(&engine, 0U);
    }
    ge_init(&restored);
    assert(ge_model_import(&restored, stored, stored_size) == GE_OK);
    assert(ge_class_count(&restored) == 2U);
    assert(memcmp(&original, ge_class_get(&restored, 0U), sizeof(original)) == 0);
    assert(ge_class_get(&restored, 1U)->template_count == 1U);
}

static void test_similarity_warning_survives_accepted_and_ready_events(void)
{
    ge_event_t event = {0};
    reset_app();
    fixture_class(0U);
    command("learn 2");
    pending_fixture();
    event.type = GE_EVENT_DEMO_WARNING;
    event.status = GE_ERR_CONFLICT;
    event.class_id = 0U;
    event.training_count = GE_TEMPLATES_PER_CLASS;
    event.distance = 0.02f;
    event.second_distance = 0.1f;
    engine.events[0] = event;
    event.type = GE_EVENT_DEMO_ACCEPTED;
    event.status = GE_OK;
    engine.events[1] = event;
    event.type = GE_EVENT_TRAIN_READY;
    engine.events[2] = event;
    engine.event_read = 0U;
    engine.event_write = 3U;
    engine.event_count = 3U;
    clear_logs();
    engine_events(mock_tick);
    assert(view.state == DISPLAY_STATE_READY && ge_training_ready(&engine));
    assert(strstr(view.message, "Similar to slot 1") != NULL);
    assert(strstr(log_history, "TRAIN,WARN,SIMILAR,1,") != NULL);
    assert(strstr(log_history, "TRAIN,READY,2,3\r\n") != NULL);
    assert(strstr(log_history, "TRAIN,REJECT,") == NULL);
    assert(!delete_confirm && store_calls == 0U && ge_class_count(&engine) == 1U);
}

static void test_key_timestamp_follows_latest_sensor_sample(void)
{
    unsigned i;
    uint32_t timestamp;
    reset_app();
    command("learn 1");
    timestamp = mock_tick;
    for (i = 0U; i < 140U; ++i) feed_sample(9.80665f, &timestamp);
    user_key_process(timestamp);
    mock_tick = timestamp + 5U;
    mock_user_down = 1U;
    ++sample_ticks;
    gesture_app_process();
    assert(capture_held && ge_training_capturing(&engine));
    mock_tick += 5U;
    mock_user_down = 0U;
    ++sample_ticks;
    gesture_app_process();
    assert(!capture_held && !ge_training_capturing(&engine));
    assert(strstr(log_history, "TRAIN,REJECT,NOT READY\r\n") == NULL);
    assert(strstr(log_history, "TRAIN,REJECT,SAMPLE GAP\r\n") == NULL);
}

static void test_optional_calibration_does_not_hide_hardware_failures(void)
{
    reset_app();
    timer_start_result = HAL_ERROR;
    timer_running = 0;
    gesture_app_init();
    assert(!timer_running && !calibrated && calibration_calls == 0U);
    assert(!(ready_mask & BOARD_READY_IMU) && view.state == DISPLAY_STATE_ERROR);
    assert(strstr(log_history, "ERROR,STARTUP,TIMER_FAILED\r\n") != NULL);
    command("learn 1");
    assert(!training && !strcmp(last_log, "ERROR,LEARN,IMU_UNAVAILABLE\r\n"));
    fixture_class(0U);
    command("arm");
    assert(!armed && !strcmp(last_log, "ERROR,ARM,IMU_UNAVAILABLE\r\n"));
    reset_app();
    board_ready_result &= ~BOARD_READY_IMU;
    timer_running = 0;
    gesture_app_init();
    assert(!timer_running && !calibrated && calibration_calls == 0U);
    assert(view.state == DISPLAY_STATE_ERROR);
    assert(strstr(log_history, "ERROR,STARTUP,IMU_UNAVAILABLE\r\n") != NULL);
    command("learn 1");
    assert(!training && !strcmp(last_log, "ERROR,LEARN,IMU_UNAVAILABLE\r\n"));
    fixture_class(0U);
    command("arm");
    assert(!armed && !strcmp(last_log, "ERROR,ARM,IMU_UNAVAILABLE\r\n"));
}

static void test_read_fault_blocks_use_until_samples_recover(void)
{
    unsigned i;
    reset_app();
    fixture_class(0U);
    command("arm");
    assert(armed && !calibrated && calibration_calls == 0U);
    imu_read_success = 0;
    for (i = 0U; i < 3U; ++i) sample_once(processed_ticks + 1U);
    assert(!armed && consecutive_errors == 3U);
    assert(strstr(log_history, "ERROR,IMU_READ\r\n") != NULL);
    command("arm");
    assert(!armed && !strcmp(last_log, "ERROR,ARM,IMU_UNAVAILABLE\r\n"));
    command("learn 2");
    assert(!training && !strcmp(last_log, "ERROR,LEARN,IMU_UNAVAILABLE\r\n"));
    imu_read_success = 1;
    sample_once(processed_ticks + 1U);
    assert(consecutive_errors == 0U);
    command("arm");
    assert(armed && !calibrated && calibration_calls == 0U);
    command("learn 2");
    assert(training && !armed && !calibrated && calibration_calls == 0U);
}

static void test_status_keeps_calibration_optional_and_truthful(void)
{
    unsigned long tick;
    unsigned status_armed, count, status_calibrated;
    reset_app();
    command("status");
    assert(sscanf(last_log, "STATUS,%lu,%u,%u,%u", &tick, &status_armed, &count, &status_calibrated) == 4);
    assert(status_calibrated == 0U && calibration_calls == 0U);
    fixture_class(0U);
    command("arm");
    command("status");
    assert(sscanf(last_log, "STATUS,%lu,%u,%u,%u", &tick, &status_armed, &count, &status_calibrated) == 4);
    assert(status_armed == 1U && count == 1U && status_calibrated == 0U);
    assert(calibration_calls == 0U);
    command("calibrate");
    command("status");
    assert(sscanf(last_log, "STATUS,%lu,%u,%u,%u", &tick, &status_armed, &count, &status_calibrated) == 4);
    assert(status_armed == 0U && status_calibrated == 1U && calibration_calls == 1U);
}

static void test_raw_reports_physical_key_outside_training(void)
{
    unsigned down, reported_key;
    unsigned long seq, timestamp;
    long ax, ay, az, gx, gy, gz;
    int consumed;
    reset_app();
    command("stream 1");
    for (down = 0U; down < 3U; ++down) {
        mock_user_down = (uint8_t)(down == 1U);
        mock_tick += 5U;
        clear_logs();
        sample_once(processed_ticks + 1U);
        assert(sscanf(last_log, "RAW,%lu,%lu,%ld,%ld,%ld,%ld,%ld,%ld,%u%n",
            &seq, &timestamp, &ax, &ay, &az, &gx, &gy, &gz, &reported_key, &consumed) == 9);
        assert(seq == down + 1U && timestamp == mock_tick);
        assert(reported_key == mock_user_down && !strcmp(last_log + consumed, "\r\n"));
        assert(ax == 0L && ay == 0L && az == 9806L && gx == 0L && gy == 0L && gz == 0L);
        assert(!training && !capture_held && !armed && !engine.recognizing);
    }
    command("stream 0");
    clear_logs();
    mock_user_down = 1U;
    sample_once(processed_ticks + 1U);
    assert(log_calls == 0U);
}

int main(void)
{
    test_arming_and_event_gate();
    test_learning_requires_explicit_confirmation();
    test_save_failure_restores_old_model();
    test_delete_cancel_and_failed_write();
    test_delete_ignores_new_capture_during_confirmation();
    test_failed_learn_cannot_keep_armed_display();
    test_timer_restart_failure_blocks_arm();
    test_inventory_restores_slots_and_pending_operation();
    test_commands_report_reasons_without_changing_pending_work();
    test_calibration_and_delete_failure_reasons();
    test_key_capture_requires_release_then_fresh_press();
    test_interrupted_and_cancelled_capture_cannot_save_fragments();
    test_user_only_records_learning_and_slot_colors();
    test_live_match_changes_rgb_without_action_event();
    test_rgb_accepted_hold_and_immediate_class_switch();
    test_unknown_indication_survives_empty_preview();
    test_one_demo_can_save_or_add_without_interrupted_save();
    test_learning_ignores_unheld_motion_and_rejects_long_hold();
    test_similar_demo_can_save_without_losing_existing_action();
    test_similarity_warning_survives_accepted_and_ready_events();
    test_key_timestamp_follows_latest_sensor_sample();
    test_optional_calibration_does_not_hide_hardware_failures();
    test_read_fault_blocks_use_until_samples_recover();
    test_status_keeps_calibration_optional_and_truthful();
    test_raw_reports_physical_key_outside_training();
    puts("Gesture app tests passed (25 cases, real engine with mocked hardware).");
    return 0;
}
