#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../App/gesture_usb.c"
#include "../USB_DEVICE/App/usbd_cdc_if.c"

USBD_HandleTypeDef hUsbDeviceHS;
static USBD_CDC_HandleTypeDef mock_cdc;
static uint32_t irq_mask;
static unsigned sent_count, command_count;
static int reconnect_before_mask;
static int reconnect_in_command;
static char sent_messages[80][LINE_SIZE];
static char received_command[80];

void __DMB(void) {}
uint32_t __get_PRIMASK(void) { return irq_mask; }
void __set_PRIMASK(uint32_t mask) { irq_mask = mask; }

static void reconnect(void)
{
    hUsbDeviceHS.dev_state = 0U;
    (void)USBD_Interface_fops_HS.DeInit();
    memset(&mock_cdc, 0, sizeof(mock_cdc));
    hUsbDeviceHS.pClassData = &mock_cdc;
    hUsbDeviceHS.dev_state = USBD_STATE_CONFIGURED;
    (void)USBD_Interface_fops_HS.Init();
}

void __disable_irq(void)
{
    /* Simulate the last possible USB interrupt before the TX critical section. */
    if (reconnect_before_mask) {
        reconnect_before_mask = 0;
        reconnect();
    }
    irq_mask = 1U;
}

uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef *device, uint8_t *buffer, uint32_t length)
{
    USBD_CDC_HandleTypeDef *cdc = (USBD_CDC_HandleTypeDef *)device->pClassData;
    assert(cdc != NULL);
    cdc->TxBuffer = buffer;
    cdc->TxLength = length;
    return USBD_OK;
}
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef *device, uint8_t *buffer)
{
    (void)device;
    assert(buffer != NULL);
    return USBD_OK;
}
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef *device)
{
    USBD_CDC_HandleTypeDef *cdc = (USBD_CDC_HandleTypeDef *)device->pClassData;
    assert(cdc != NULL && cdc->TxState == 0U);
    assert(cdc->TxLength < LINE_SIZE && sent_count < 80U);
    memcpy(sent_messages[sent_count], cdc->TxBuffer, cdc->TxLength);
    sent_messages[sent_count][cdc->TxLength] = '\0';
    ++sent_count;
    cdc->TxState = 1U;
    return USBD_OK;
}
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device) { (void)device; return USBD_OK; }

static void handle_command(const char *text)
{
    ++command_count;
    (void)snprintf(received_command, sizeof(received_command), "%s", text);
    if (reconnect_in_command) {
        reconnect_in_command = 0;
        reconnect();
    }
}

static void reset_usb(void)
{
    memset(&hUsbDeviceHS, 0, sizeof(hUsbDeviceHS));
    memset(&mock_cdc, 0, sizeof(mock_cdc));
    memset(sent_messages, 0, sizeof(sent_messages));
    memset(received_command, 0, sizeof(received_command));
    tx_read = tx_write = tx_count = 0U;
    rx_read = rx_write = 0U;
    rx_overflow = 0U;
    session_epoch = processed_epoch = 0U;
    command_len = discard_line = was_connected = 0U;
    dropped = 0U;
    irq_mask = sent_count = command_count = 0U;
    reconnect_before_mask = reconnect_in_command = 0;
    gesture_usb_init(handle_command);
}

static void connect_ready(void)
{
    reconnect();
    gesture_usb_process();
    assert(sent_count == 1U && strncmp(sent_messages[0], "HELLO,", 6U) == 0);
    mock_cdc.TxState = 0U;
    sent_count = 0U;
}

static void test_cdc_null_and_not_configured_guard(void)
{
    uint8_t text[] = "sample";
    reset_usb();
    hUsbDeviceHS.dev_state = USBD_STATE_CONFIGURED;
    assert(CDC_Transmit_HS(text, sizeof(text)) == USBD_FAIL);
    hUsbDeviceHS.pClassData = &mock_cdc;
    hUsbDeviceHS.dev_state = 0U;
    assert(CDC_Transmit_HS(text, sizeof(text)) == USBD_FAIL);
    gesture_usb_log(0, "EVENT,OLD\r\n");
    assert(tx_count == 0U && sent_count == 0U);
}

static void test_visible_disconnect_drops_old_events(void)
{
    reset_usb();
    connect_ready();
    gesture_usb_log(0, "EVENT,OLD\r\n");
    hUsbDeviceHS.dev_state = 0U;
    (void)USBD_Interface_fops_HS.DeInit();
    gesture_usb_process();
    assert(tx_count == 0U && sent_count == 0U);
    reconnect();
    gesture_usb_process();
    assert(sent_count == 1U && strncmp(sent_messages[0], "HELLO,", 6U) == 0);
}

static void test_reconnect_between_polls_drops_old_work(void)
{
    static const uint8_t line[] = "arm\n";
    reset_usb();
    connect_ready();
    gesture_usb_log(0, "EVENT,OLD\r\n");
    gesture_usb_receive(line, sizeof(line) - 1U);
    reconnect();
    gesture_usb_process();
    assert(command_count == 0U && tx_count == 0U);
    assert(sent_count == 1U && strncmp(sent_messages[0], "HELLO,", 6U) == 0);
}

static void test_reconnect_immediately_before_tx(void)
{
    reset_usb();
    connect_ready();
    gesture_usb_log(0, "EVENT,OLD\r\n");
    reconnect_before_mask = 1;
    gesture_usb_process();
    assert(sent_count == 0U);
    gesture_usb_process();
    assert(sent_count == 1U && strncmp(sent_messages[0], "HELLO,", 6U) == 0);
}

static void test_reconnect_during_command_discards_remaining_lines(void)
{
    static const uint8_t lines[] = "calibrate\narm\n";
    reset_usb();
    connect_ready();
    gesture_usb_receive(lines, sizeof(lines) - 1U);
    reconnect_in_command = 1;
    gesture_usb_process();
    gesture_usb_process();
    assert(command_count == 1U && strcmp(received_command, "calibrate") == 0);
}

static void test_active_packet_is_stable_and_raw_backpressure(void)
{
    char snapshot[LINE_SIZE];
    unsigned i;
    reset_usb();
    connect_ready();
    gesture_usb_log(0, "EVENT,KEPT\r\n");
    gesture_usb_process();
    assert(mock_cdc.TxState == 1U && sent_count == 1U);
    memcpy(snapshot, mock_cdc.TxBuffer, mock_cdc.TxLength);
    for (i = 0U; i < 20U; ++i) gesture_usb_log(1, "RAW,%u\r\n", i);
    gesture_usb_process();
    assert(memcmp(snapshot, mock_cdc.TxBuffer, mock_cdc.TxLength) == 0);
    assert(sent_count == 1U && gesture_usb_drops() == 12U);
    gesture_usb_log(0, "EVENT,PRIORITY\r\n");
    assert(tx_count == 9U);
}

static void test_line_parser_rejects_overflow_and_control_bytes(void)
{
    uint8_t garbage[300];
    static const uint8_t valid[] = "\nstatus\n";
    static const uint8_t invalid[] = {'a', 0U, 'r', 'm', '\n'};
    unsigned i;
    reset_usb();
    connect_ready();
    memset(garbage, 'x', sizeof(garbage));
    gesture_usb_receive(garbage, sizeof(garbage));
    gesture_usb_process();
    assert(command_count == 0U);
    gesture_usb_receive(valid, sizeof(valid) - 1U);
    for (i = 0U; i < 4U; ++i) gesture_usb_process();
    assert(command_count == 1U && strcmp(received_command, "status") == 0);
    gesture_usb_receive(invalid, sizeof(invalid));
    gesture_usb_process();
    assert(command_count == 1U);
}

static void test_rgb_timing_configuration_crosses_usb_packet_boundary(void)
{
    static const uint8_t line[] =
        "configure 8 3 ffffff ffffff ffffff ffffff ffffff ffffff ffffff ffffff 30000\n";
    reset_usb();
    connect_ready();
    assert(sizeof(line) - 1U == 76U);
    gesture_usb_receive(line, 64U);
    gesture_usb_process();
    assert(command_count == 0U);
    gesture_usb_receive(line + 64U, sizeof(line) - 1U - 64U);
    gesture_usb_process();
    assert(command_count == 1U && strlen(received_command) == 75U);
    assert(!memcmp(received_command, line, 75U));
}

int main(void)
{
    test_cdc_null_and_not_configured_guard();
    test_visible_disconnect_drops_old_events();
    test_reconnect_between_polls_drops_old_work();
    test_reconnect_immediately_before_tx();
    test_reconnect_during_command_discards_remaining_lines();
    test_active_packet_is_stable_and_raw_backpressure();
    test_line_parser_rejects_overflow_and_control_bytes();
    test_rgb_timing_configuration_crosses_usb_packet_boundary();
    puts("Gesture USB tests passed (8 cases, including generated CDC callbacks).");
    return 0;
}
