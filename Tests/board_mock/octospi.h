#ifndef BOARD_MOCK_OCTOSPI_H
#define BOARD_MOCK_OCTOSPI_H
#include <stdint.h>

typedef int OSPI_HandleTypeDef;
typedef enum {HAL_OK = 0, HAL_ERROR = 1} HAL_StatusTypeDef;
typedef struct {
    uint32_t OperationType, FlashId, Instruction, InstructionMode, InstructionSize;
    uint32_t InstructionDtrMode, Address, AddressMode, AddressSize, AddressDtrMode;
    uint32_t AlternateBytesMode, AlternateBytesDtrMode, DataMode, NbData;
    uint32_t DataDtrMode, DummyCycles, DQSMode, SIOOMode;
} OSPI_RegularCmdTypeDef;
typedef struct {uint32_t Pin, Mode, Pull, Speed;} GPIO_InitTypeDef;

#define HAL_OSPI_OPTYPE_COMMON_CFG 0u
#define HAL_OSPI_FLASH_ID_1 0u
#define HAL_OSPI_INSTRUCTION_1_LINE 1u
#define HAL_OSPI_INSTRUCTION_8_BITS 8u
#define HAL_OSPI_INSTRUCTION_DTR_DISABLE 0u
#define HAL_OSPI_ADDRESS_1_LINE 1u
#define HAL_OSPI_ADDRESS_NONE 0u
#define HAL_OSPI_ADDRESS_24_BITS 24u
#define HAL_OSPI_ADDRESS_DTR_DISABLE 0u
#define HAL_OSPI_ALTERNATE_BYTES_NONE 0u
#define HAL_OSPI_ALTERNATE_BYTES_DTR_DISABLE 0u
#define HAL_OSPI_DATA_1_LINE 1u
#define HAL_OSPI_DATA_NONE 0u
#define HAL_OSPI_DATA_DTR_DISABLE 0u
#define HAL_OSPI_DQS_DISABLE 0u
#define HAL_OSPI_SIOO_INST_EVERY_CMD 0u
#define GPIOA 0
#define GPIO_PIN_1 2u
#define GPIO_PIN_3 8u
#define GPIO_PIN_SET 1u
#define GPIO_MODE_OUTPUT_PP 0u
#define GPIO_PULLUP 1u
#define GPIO_SPEED_FREQ_LOW 0u

extern OSPI_HandleTypeDef hospi2;
HAL_StatusTypeDef HAL_OSPI_Command(OSPI_HandleTypeDef *, OSPI_RegularCmdTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_OSPI_Receive(OSPI_HandleTypeDef *, uint8_t *, uint32_t);
HAL_StatusTypeDef HAL_OSPI_Transmit(OSPI_HandleTypeDef *, uint8_t *, uint32_t);
HAL_StatusTypeDef HAL_OSPI_Abort(OSPI_HandleTypeDef *);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t);
void HAL_GPIO_WritePin(int, uint32_t, uint32_t);
void HAL_GPIO_Init(int, GPIO_InitTypeDef *);
#endif
