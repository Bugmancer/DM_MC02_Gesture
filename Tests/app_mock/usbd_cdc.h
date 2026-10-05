#ifndef GESTURE_TEST_USBD_CDC_H
#define GESTURE_TEST_USBD_CDC_H
#include <stdint.h>
#define USBD_OK 0U
#define USBD_BUSY 1U
#define USBD_FAIL 2U
#define USBD_STATE_CONFIGURED 3U
#define CDC_SEND_ENCAPSULATED_COMMAND 0x00U
#define CDC_GET_ENCAPSULATED_RESPONSE 0x01U
#define CDC_SET_COMM_FEATURE 0x02U
#define CDC_GET_COMM_FEATURE 0x03U
#define CDC_CLEAR_COMM_FEATURE 0x04U
#define CDC_SET_LINE_CODING 0x20U
#define CDC_GET_LINE_CODING 0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U
#define CDC_SEND_BREAK 0x23U
typedef struct {
    volatile uint32_t TxState;
    uint8_t *TxBuffer;
    uint32_t TxLength;
} USBD_CDC_HandleTypeDef;
typedef struct {
    uint8_t dev_state;
    void *pClassData;
} USBD_HandleTypeDef;
typedef struct {
    int8_t (*Init)(void);
    int8_t (*DeInit)(void);
    int8_t (*Control)(uint8_t command, uint8_t *buffer, uint16_t length);
    int8_t (*Receive)(uint8_t *buffer, uint32_t *length);
    int8_t (*TransmitCplt)(uint8_t *buffer, uint32_t *length, uint8_t epnum);
} USBD_CDC_ItfTypeDef;
uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef *device, uint8_t *buffer, uint32_t length);
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef *device, uint8_t *buffer);
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef *device);
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device);
#endif
