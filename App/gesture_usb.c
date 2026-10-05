#include "gesture_usb.h"
#include "main.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceHS;

#define TX_COUNT 16u
#define LINE_SIZE 192u
#define RX_SIZE 256u
static char tx[TX_COUNT][LINE_SIZE];
static char active_tx[LINE_SIZE];
static uint16_t tx_len[TX_COUNT];
static uint8_t tx_read, tx_write, tx_count;
static volatile uint16_t rx_read, rx_write;
static volatile uint8_t rx_overflow;
static volatile uint32_t session_epoch;
static uint32_t processed_epoch;
static uint8_t rx[RX_SIZE];
static char command[80];
static uint8_t command_len, discard_line, was_connected;
static uint32_t dropped;
static gesture_command_fn command_handler;

int gesture_usb_connected(void)
{
    return hUsbDeviceHS.dev_state == USBD_STATE_CONFIGURED && hUsbDeviceHS.pClassData != NULL;
}

void gesture_usb_init(gesture_command_fn handler)
{
    command_handler = handler;
}

void gesture_usb_session_changed(void) { ++session_epoch; }

void gesture_usb_receive(const uint8_t *data, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; ++i) {
        uint16_t next = (uint16_t)((rx_write + 1u) % RX_SIZE);
        if (next == rx_read) {
            rx_overflow = 1u;
        } else if (!rx_overflow) {
            rx[rx_write] = data[i];
            __DMB();
            rx_write = next;
        }
    }
}

void gesture_usb_log(int raw, const char *format, ...)
{
    int length;
    va_list args;
    if (!gesture_usb_connected()) return;
    if (tx_count >= (raw ? TX_COUNT / 2u : TX_COUNT)) {
        ++dropped;
        return;
    }
    va_start(args, format);
    length = vsnprintf(tx[tx_write], LINE_SIZE, format, args);
    va_end(args);
    if (length <= 0 || length >= (int)LINE_SIZE) {
        ++dropped;
        return;
    }
    tx_len[tx_write] = (uint16_t)length;
    tx_write = (uint8_t)((tx_write + 1u) % TX_COUNT);
    ++tx_count;
}

void gesture_usb_process(void)
{
    uint32_t mask;
    unsigned budget = 80u;
    int connected = gesture_usb_connected();
    if (connected != was_connected || processed_epoch != session_epoch) {
        /* Never replay pre-disconnect gesture events to a newly connected host. */
        tx_read = tx_write = tx_count = 0u;
        command_len = discard_line = 0u;
        mask = __get_PRIMASK();
        __disable_irq();
        rx_read = rx_write;
        rx_overflow = 0u;
        processed_epoch = session_epoch;
        __set_PRIMASK(mask);
        was_connected = (uint8_t)connected;
        if (connected) gesture_usb_log(0, "HELLO,DM_MC02_GESTURE,1\r\n");
    }
    if (!connected) return;
    if (rx_overflow) {
        mask = __get_PRIMASK();
        __disable_irq();
        rx_read = rx_write;
        rx_overflow = 0u;
        __set_PRIMASK(mask);
        command_len = 0u;
        discard_line = 1u;
        gesture_usb_log(0, "ERROR,RX_OVERFLOW\r\n");
    }
    while (rx_read != rx_write && budget--) {
        char c = (char)rx[rx_read];
        rx_read = (uint16_t)((rx_read + 1u) % RX_SIZE);
        if (c == '\r') continue;
        if (c == '\n') {
            if (!discard_line && command_len && command_handler) {
                command[command_len] = '\0';
                command_handler(command);
                if (processed_epoch != session_epoch) return;
            }
            command_len = discard_line = 0u;
        } else if ((unsigned char)c < 32u || (unsigned char)c > 126u) {
            discard_line = 1u;
        } else if (!discard_line) {
            if (command_len < sizeof(command) - 1u) command[command_len++] = c;
            else discard_line = 1u;
        }
    }
    mask = __get_PRIMASK();
    __disable_irq();
    if (tx_count && gesture_usb_connected() && processed_epoch == session_epoch) {
        USBD_CDC_HandleTypeDef *cdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceHS.pClassData;
        if (cdc->TxState == 0u) {
            uint16_t length = tx_len[tx_read];
            memcpy(active_tx, tx[tx_read], length);
            if (CDC_Transmit_HS((uint8_t *)active_tx, length) == USBD_OK) {
                tx_read = (uint8_t)((tx_read + 1u) % TX_COUNT);
                --tx_count;
            }
        }
    }
    __set_PRIMASK(mask);
}

uint32_t gesture_usb_drops(void) { return dropped; }
