#ifndef GESTURE_APP_TEST_MAIN_H
#define GESTURE_APP_TEST_MAIN_H
#include <stdint.h>
typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
typedef struct { uint32_t instance; } TIM_HandleTypeDef;
typedef struct { uint32_t CYCCNT, CTRL; } MockDwt;
typedef struct { uint32_t DEMCR; } MockCoreDebug;
extern MockDwt mock_dwt;
extern MockCoreDebug mock_core_debug;
extern uint32_t SystemCoreClock;
#define DWT (&mock_dwt)
#define CoreDebug (&mock_core_debug)
#define CoreDebug_DEMCR_TRCENA_Msk 1U
#define DWT_CTRL_CYCCNTENA_Msk 1U
#define UNUSED(value) ((void)(value))
void __DMB(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *timer);
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *timer);
#endif
