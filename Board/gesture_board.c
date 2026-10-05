#include "gesture_board.h"
#include "main.h"
#include "adc.h"
#include "spi.h"

#include <math.h>
#include <string.h>

#define IMU_TIMEOUT_MS 3u
#define ACCEL_SCALE (12.0f * 9.80665f / 32768.0f)
#define GYRO_SCALE (2000.0f * 0.0174532925199433f / 32768.0f)

int board_store_init(void);

static uint32_t ready;
static float gyro_bias[3];
static BoardKey key_candidate;
static BoardKey key_stable;
static uint32_t key_change_ms;
static uint32_t key_poll_ms;
static uint8_t user_candidate, user_stable;
static uint32_t user_change_ms;

static int imu_read(int accel, uint8_t reg, uint8_t *data, uint16_t count)
{
    uint8_t tx[10] = {0};
    uint8_t rx[10];
    uint16_t prefix = accel ? 2u : 1u;
    GPIO_TypeDef *port = accel ? ACC_CS_GPIO_Port : GYRO_CS_GPIO_Port;
    uint16_t pin = accel ? ACC_CS_Pin : GYRO_CS_Pin;
    HAL_StatusTypeDef status;
    if (count + prefix > sizeof(tx)) return 0;
    tx[0] = reg | 0x80u;
    /* The accelerometer clocks out a dummy byte after the read address. */
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    status = HAL_SPI_TransmitReceive(&hspi2, tx, rx, count + prefix, IMU_TIMEOUT_MS);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    if (status != HAL_OK) return 0;
    memcpy(data, rx + prefix, count);
    return 1;
}

static int imu_write(int accel, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    uint8_t rx[2];
    GPIO_TypeDef *port = accel ? ACC_CS_GPIO_Port : GYRO_CS_GPIO_Port;
    uint16_t pin = accel ? ACC_CS_Pin : GYRO_CS_Pin;
    HAL_StatusTypeDef status;
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    status = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 2u, IMU_TIMEOUT_MS);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    HAL_Delay(1u);
    return status == HAL_OK;
}

static int imu_config(int accel, uint8_t reg, uint8_t value, uint8_t mask)
{
    uint8_t readback;
    return imu_write(accel, reg, value) && imu_read(accel, reg, &readback, 1u) &&
           (readback & mask) == (value & mask);
}

static int imu_init(void)
{
    uint8_t id;
    HAL_GPIO_WritePin(ACC_CS_GPIO_Port, ACC_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GYRO_CS_GPIO_Port, GYRO_CS_Pin, GPIO_PIN_SET);
    /* PC2_C and PC3_C must connect to the digital PC2/PC3 pads. */
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC2, SYSCFG_SWITCH_PC2_CLOSE);
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC3, SYSCFG_SWITCH_PC3_CLOSE);
    HAL_Delay(20u);
    if (!imu_read(1, 0x00u, &id, 1u)) return 0;
    HAL_Delay(1u); /* First CS transition selects accelerometer SPI mode. */
    if (!imu_write(1, 0x7eu, 0xb6u) || !imu_write(0, 0x14u, 0xb6u)) return 0;
    HAL_Delay(80u);
    if (!imu_read(1, 0x00u, &id, 1u)) return 0;
    HAL_Delay(1u);
    if (!imu_read(1, 0x00u, &id, 1u) || id != 0x1eu) return 0;
    if (!imu_read(0, 0x00u, &id, 1u) || id != 0x0fu) return 0;
    if (!imu_config(1, 0x7cu, 0x00u, 0xffu)) return 0;
    HAL_Delay(5u);
    if (!imu_config(1, 0x7du, 0x04u, 0xffu)) return 0;
    HAL_Delay(50u);
    /* 400 Hz / OSR4 acceleration and 400 Hz / 47 Hz gyro avoid duplicate
       samples at the foreground 200 Hz cadence and attenuate aliasing. */
    if (!imu_config(1, 0x40u, 0x8au, 0xffu) ||
        !imu_config(1, 0x41u, 0x02u, 0x03u) ||
        !imu_config(0, 0x0fu, 0x00u, 0x07u) ||
        !imu_config(0, 0x10u, 0x83u, 0x0fu) ||
        !imu_config(0, 0x11u, 0x00u, 0xffu)) return 0;
    /* Sampling is timer-driven; leave the sensor interrupt outputs disabled. */
    if (!imu_config(1, 0x58u, 0x00u, 0xffu) ||
        !imu_config(0, 0x15u, 0x00u, 0x80u)) return 0;
    HAL_Delay(30u);
    return 1;
}

uint32_t board_init(void)
{
    ready = 0u;
    memset(gyro_bias, 0, sizeof(gyro_bias));
    key_candidate = BOARD_KEY_NONE;
    key_stable = BOARD_KEY_NONE;
    key_poll_ms = HAL_GetTick();
    key_change_ms = key_poll_ms;
    user_candidate = user_stable = (uint8_t)(HAL_GPIO_ReadPin(USER_KEY_GPIO_Port, USER_KEY_Pin) == GPIO_PIN_RESET);
    user_change_ms = key_poll_ms;
    hspi6.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    if (HAL_SPI_Init(&hspi6) == HAL_OK) board_rgb(0u, 0u, 0u);
    if (imu_init()) ready |= BOARD_READY_IMU;
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) == HAL_OK)
        ready |= BOARD_READY_KEYS;
    if (board_store_init()) ready |= BOARD_READY_FLASH;
    return ready;
}

int board_read_imu(BoardImuSample *sample)
{
    uint8_t acc[6], gyro[6];
    unsigned int axis;
    uint32_t timestamp = HAL_GetTick();
    if (sample == NULL || (ready & BOARD_READY_IMU) == 0u) return 0;
    if (!imu_read(1, 0x12u, acc, sizeof(acc)) ||
        !imu_read(0, 0x02u, gyro, sizeof(gyro))) return 0;
    sample->timestamp_ms = timestamp;
    for (axis = 0u; axis < 3u; ++axis) {
        int16_t a = (int16_t)((uint16_t)acc[axis * 2u] |
                             ((uint16_t)acc[axis * 2u + 1u] << 8));
        int16_t g = (int16_t)((uint16_t)gyro[axis * 2u] |
                             ((uint16_t)gyro[axis * 2u + 1u] << 8));
        sample->accel[axis] = (float)a * ACCEL_SCALE;
        sample->gyro[axis] = (float)g * GYRO_SCALE - gyro_bias[axis];
    }
    return 1;
}

int board_calibrate_gyro(uint32_t duration_ms)
{
    float mean_a[3] = {0}, mean_g[3] = {0};
    float m2_a[3] = {0}, m2_g[3] = {0};
    uint32_t start, count = 0u;
    unsigned int axis;
    if (duration_ms < 500u || duration_ms > 5000u) return 0;
    start = HAL_GetTick();
    do {
        BoardImuSample sample;
        float norm_a = 0.0f;
        if (!board_read_imu(&sample)) return 0;
        ++count;
        for (axis = 0u; axis < 3u; ++axis) {
            float raw_g = sample.gyro[axis] + gyro_bias[axis];
            float da = sample.accel[axis] - mean_a[axis];
            float dg = raw_g - mean_g[axis];
            if (fabsf(raw_g) > 0.15f) return 0;
            norm_a += sample.accel[axis] * sample.accel[axis];
            mean_a[axis] += da / (float)count;
            mean_g[axis] += dg / (float)count;
            m2_a[axis] += da * (sample.accel[axis] - mean_a[axis]);
            m2_g[axis] += dg * (raw_g - mean_g[axis]);
        }
        if (norm_a < 75.0f || norm_a > 120.0f) return 0;
        HAL_Delay(5u);
    } while ((uint32_t)(HAL_GetTick() - start) < duration_ms);
    if (count < 80u) return 0;
    for (axis = 0u; axis < 3u; ++axis) {
        if (m2_a[axis] / (float)count > 0.04f ||
            m2_g[axis] / (float)count > 0.0001f) return 0;
    }
    memcpy(gyro_bias, mean_g, sizeof(gyro_bias));
    return 1;
}

static BoardKey adc_key(uint32_t value)
{
    static const uint16_t centers[] = {50u, 13000u, 26100u, 39100u, 52200u};
    static const BoardKey keys[] = {
        BOARD_KEY_OK, BOARD_KEY_DOWN, BOARD_KEY_UP, BOARD_KEY_LEFT, BOARD_KEY_RIGHT
    };
    unsigned int index;
    for (index = 0u; index < sizeof(centers) / sizeof(centers[0]); ++index) {
        uint32_t difference = value > centers[index] ? value - centers[index] : centers[index] - value;
        if (difference <= 1000u) return keys[index];
    }
    return BOARD_KEY_NONE;
}

BoardKey board_poll_key(uint32_t now_ms)
{
    BoardKey observed = BOARD_KEY_NONE;
    BoardKey event = BOARD_KEY_NONE;
    uint8_t user_down, user_pressed = 0u;
    if ((uint32_t)(now_ms - key_poll_ms) < 5u) return BOARD_KEY_NONE;
    key_poll_ms = now_ms;
    user_down = (uint8_t)(HAL_GPIO_ReadPin(USER_KEY_GPIO_Port, USER_KEY_Pin) == GPIO_PIN_RESET);
    if (user_down != user_candidate) {
        user_candidate = user_down;
        user_change_ms = now_ms;
    }
    if ((uint32_t)(now_ms - user_change_ms) >= 40u && user_down != user_stable) {
        user_stable = user_down;
        user_pressed = user_down;
    }
    if ((ready & BOARD_READY_KEYS) != 0u && HAL_ADC_Start(&hadc1) == HAL_OK) {
        if (HAL_ADC_PollForConversion(&hadc1, 1u) == HAL_OK)
            observed = adc_key(HAL_ADC_GetValue(&hadc1));
        (void)HAL_ADC_Stop(&hadc1);
    }
    if (observed != key_candidate) {
        key_candidate = observed;
        key_change_ms = now_ms;
    }
    if ((uint32_t)(now_ms - key_change_ms) >= 40u && observed != key_stable) {
        key_stable = observed;
        event = observed;
    }
    return event != BOARD_KEY_NONE ? event : (user_pressed ? BOARD_KEY_USER : BOARD_KEY_NONE);
}

uint8_t board_user_key_down(void) { return user_stable; }

void board_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t data[504] = {0};
    uint8_t grb[3] = {green, red, blue};
    unsigned int color, bit;
    /* At 6 MHz: 0 = 0.333 us high; 1 = 0.667 us high. The 240-byte
       zero prefixes/suffixes each provide a 320 us low reset interval. */
    for (color = 0u; color < 3u; ++color) {
        for (bit = 0u; bit < 8u; ++bit)
            data[240u + color * 8u + bit] = (grb[color] & (0x80u >> bit)) ? 0xf0u : 0xc0u;
    }
    (void)HAL_SPI_Transmit(&hspi6, data, sizeof(data), 5u);
}
