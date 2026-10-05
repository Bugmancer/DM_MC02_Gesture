#include <assert.h>
#include <stdio.h>
#include "../Board/gesture_board.c"

GPIO_TypeDef mock_gpio;
ADC_HandleTypeDef hadc1;
SPI_HandleTypeDef hspi2, hspi6;
static uint32_t mock_tick, mock_adc;
static uint8_t mock_pressed, mock_adc_ok;

uint32_t HAL_GetTick(void) { return mock_tick; }
void HAL_Delay(uint32_t delay) { mock_tick += delay; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    assert(port == USER_KEY_GPIO_Port && pin == USER_KEY_Pin);
    return mock_pressed ? GPIO_PIN_RESET : GPIO_PIN_SET;
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{ (void)port; (void)pin; (void)state; }
void HAL_SYSCFG_AnalogSwitchConfig(uint32_t selection, uint32_t state)
{ (void)selection; (void)state; }
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi) { (void)spi; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *spi, uint8_t *tx, uint8_t *rx, uint16_t count, uint32_t timeout)
{ (void)spi; (void)tx; (void)rx; (void)count; (void)timeout; return HAL_ERROR; }
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t count, uint32_t timeout)
{ (void)spi; (void)data; (void)count; (void)timeout; return HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc, uint32_t mode, uint32_t ended)
{ (void)adc; (void)mode; (void)ended; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *adc) { (void)adc; return mock_adc_ok ? HAL_OK : HAL_ERROR; }
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *adc) { (void)adc; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *adc, uint32_t timeout)
{ (void)adc; (void)timeout; return mock_adc_ok ? HAL_OK : HAL_ERROR; }
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *adc) { (void)adc; return mock_adc; }
int board_store_init(void) { return 1; }

static void reset_keys(void)
{
    ready = BOARD_READY_KEYS;
    key_candidate = key_stable = BOARD_KEY_NONE;
    user_candidate = user_stable = 0U;
    key_change_ms = key_poll_ms = user_change_ms = mock_tick = 0U;
    mock_pressed = 0U;
    mock_adc_ok = 1U;
    mock_adc = 65535U;
}

static void test_press_release_debounce_boundaries(void)
{
    reset_keys();
    mock_pressed = 1U;
    assert(board_poll_key(4U) == BOARD_KEY_NONE);
    assert(board_poll_key(5U) == BOARD_KEY_NONE);
    assert(board_poll_key(40U) == BOARD_KEY_NONE);
    assert(board_poll_key(44U) == BOARD_KEY_NONE);
    assert(!board_user_key_down());
    assert(board_poll_key(45U) == BOARD_KEY_USER);
    assert(board_user_key_down());
    assert(board_poll_key(60U) == BOARD_KEY_NONE);
    mock_pressed = 0U;
    assert(board_poll_key(65U) == BOARD_KEY_NONE);
    assert(board_poll_key(100U) == BOARD_KEY_NONE);
    assert(board_poll_key(104U) == BOARD_KEY_NONE);
    assert(board_user_key_down());
    assert(board_poll_key(105U) == BOARD_KEY_NONE);
    assert(!board_user_key_down());
}

static void test_bounce_does_not_split_a_hold(void)
{
    reset_keys();
    mock_pressed = 1U;
    assert(board_poll_key(5U) == BOARD_KEY_NONE);
    mock_pressed = 0U;
    assert(board_poll_key(20U) == BOARD_KEY_NONE);
    mock_pressed = 1U;
    assert(board_poll_key(25U) == BOARD_KEY_NONE);
    assert(board_poll_key(60U) == BOARD_KEY_NONE);
    assert(board_poll_key(65U) == BOARD_KEY_USER);
    mock_pressed = 0U;
    assert(board_poll_key(70U) == BOARD_KEY_NONE);
    mock_pressed = 1U;
    assert(board_poll_key(95U) == BOARD_KEY_NONE);
    assert(board_user_key_down());
    mock_pressed = 0U;
    assert(board_poll_key(100U) == BOARD_KEY_NONE);
    assert(board_poll_key(135U) == BOARD_KEY_NONE);
    assert(board_user_key_down());
    assert(board_poll_key(140U) == BOARD_KEY_NONE);
    assert(!board_user_key_down());
}

static void test_adc_keys_cannot_hide_user_release(void)
{
    reset_keys();
    mock_pressed = 1U;
    assert(board_poll_key(5U) == BOARD_KEY_NONE);
    mock_adc = 26100U;
    assert(board_poll_key(10U) == BOARD_KEY_NONE);
    assert(board_poll_key(45U) == BOARD_KEY_USER);
    assert(board_poll_key(50U) == BOARD_KEY_UP);
    mock_pressed = 0U;
    mock_adc = 50U;
    assert(board_poll_key(70U) == BOARD_KEY_NONE);
    assert(board_poll_key(105U) == BOARD_KEY_NONE);
    assert(board_user_key_down());
    assert(board_poll_key(110U) == BOARD_KEY_OK);
    assert(!board_user_key_down());
    mock_pressed = 1U;
    assert(board_poll_key(120U) == BOARD_KEY_NONE);
    assert(board_poll_key(160U) == BOARD_KEY_USER);
    mock_adc_ok = 0U;
    mock_pressed = 0U;
    assert(board_poll_key(170U) == BOARD_KEY_NONE);
    assert(board_poll_key(210U) == BOARD_KEY_NONE);
    assert(!board_user_key_down());
}

static void test_tick_wrap_and_boot_hold(void)
{
    reset_keys();
    key_poll_ms = UINT32_MAX - 10U;
    mock_pressed = 1U;
    assert(board_poll_key(UINT32_MAX - 5U) == BOARD_KEY_NONE);
    assert(board_poll_key(4U) == BOARD_KEY_NONE);
    assert(board_poll_key(34U) == BOARD_KEY_USER);
    mock_pressed = 0U;
    assert(board_poll_key(39U) == BOARD_KEY_NONE);
    assert(board_poll_key(79U) == BOARD_KEY_NONE);
    assert(!board_user_key_down());
    mock_pressed = 1U;
    (void)board_init();
    assert(board_user_key_down());
    assert(board_poll_key(mock_tick + 50U) != BOARD_KEY_USER);
}

static void test_simultaneous_press_preserves_adc_cancel(void)
{
    reset_keys();
    mock_pressed = 1U;
    mock_adc = 13000U;
    assert(board_poll_key(5U) == BOARD_KEY_NONE);
    assert(board_poll_key(45U) == BOARD_KEY_DOWN);
    assert(board_user_key_down());
    assert(board_poll_key(50U) == BOARD_KEY_NONE);
}

int main(void)
{
    test_press_release_debounce_boundaries();
    test_bounce_does_not_split_a_hold();
    test_adc_keys_cannot_hide_user_release();
    test_tick_wrap_and_boot_hold();
    test_simultaneous_press_preserves_adc_cancel();
    puts("Gesture key tests passed (5 cases, real board debounce with mocked HAL).");
    return 0;
}
