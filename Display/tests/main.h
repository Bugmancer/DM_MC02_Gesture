#ifndef DISPLAY_TEST_MAIN_H
#define DISPLAY_TEST_MAIN_H
#include <stdint.h>
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
typedef enum { HAL_SPI_STATE_READY = 0, HAL_SPI_STATE_BUSY_TX = 1 } HAL_SPI_StateTypeDef;
typedef struct { uint32_t unused; } SPI_HandleTypeDef;
typedef struct { uint32_t CCR; } SCB_Type;
extern SCB_Type mock_scb;
#define SCB (&mock_scb)
#define SCB_CCR_DC_Msk (1UL << 16)
#define HAL_SPI_ERROR_NONE 0U
#define LCD_CS_GPIO_Port 1U
#define LCD_CS_Pin 1U
#define LCD_DC_GPIO_Port 2U
#define LCD_DC_Pin 1U
#define LCD_BLK_GPIO_Port 3U
#define LCD_BLK_Pin 1U
#define LCD_RES_GPIO_Port 4U
#define LCD_RES_Pin 1U
void HAL_GPIO_WritePin(uint32_t port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t size);
HAL_SPI_StateTypeDef HAL_SPI_GetState(SPI_HandleTypeDef *spi);
uint32_t HAL_SPI_GetError(SPI_HandleTypeDef *spi);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t delay);
void SCB_CleanDCache_by_Addr(uint32_t *address, int32_t size);
#endif
