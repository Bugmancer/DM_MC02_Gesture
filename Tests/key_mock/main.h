#ifndef GESTURE_KEY_TEST_MAIN_H
#define GESTURE_KEY_TEST_MAIN_H
#include <stdint.h>
typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef struct { unsigned unused; } GPIO_TypeDef;
typedef struct { unsigned unused; } ADC_HandleTypeDef;
typedef struct { struct { unsigned MasterKeepIOState; } Init; } SPI_HandleTypeDef;
extern GPIO_TypeDef mock_gpio;
#define ACC_CS_GPIO_Port (&mock_gpio)
#define GYRO_CS_GPIO_Port (&mock_gpio)
#define USER_KEY_GPIO_Port (&mock_gpio)
#define ACC_CS_Pin 1U
#define GYRO_CS_Pin 2U
#define USER_KEY_Pin 4U
#define SYSCFG_SWITCH_PC2 2U
#define SYSCFG_SWITCH_PC3 3U
#define SYSCFG_SWITCH_PC2_CLOSE 0U
#define SYSCFG_SWITCH_PC3_CLOSE 0U
#define SPI_MASTER_KEEP_IO_STATE_ENABLE 1U
#define ADC_CALIB_OFFSET 0U
#define ADC_SINGLE_ENDED 0U
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t delay);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
void HAL_SYSCFG_AnalogSwitchConfig(uint32_t selection, uint32_t state);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *spi, uint8_t *tx, uint8_t *rx, uint16_t count, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t count, uint32_t timeout);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc, uint32_t mode, uint32_t ended);
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *adc, uint32_t timeout);
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *adc);
#endif
