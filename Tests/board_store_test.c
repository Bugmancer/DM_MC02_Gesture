#include "gesture_board.h"
#include "octospi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BASE 0x7e0000u
#define SLOT 65536u

int board_store_init(void);
OSPI_HandleTypeDef hospi2;
static uint8_t nor[SLOT * 2u], baseline[SLOT * 2u];
static OSPI_RegularCmdTypeDef pending;
static uint32_t ticks;
static int powered = 1, write_enabled, cut_after = -1, read_failure;

uint32_t HAL_GetTick(void) {return ticks;}
void HAL_Delay(uint32_t duration) {ticks += duration;}
void HAL_GPIO_WritePin(int port, uint32_t pin, uint32_t value)
{(void)port; (void)pin; (void)value;}
void HAL_GPIO_Init(int port, GPIO_InitTypeDef *pins) {(void)port; (void)pins;}
HAL_StatusTypeDef HAL_OSPI_Abort(OSPI_HandleTypeDef *handle) {(void)handle; return HAL_OK;}

static int mutate(void)
{
    if (cut_after == 0) {powered = 0; return 0;}
    if (cut_after > 0) --cut_after;
    return 1;
}

HAL_StatusTypeDef HAL_OSPI_Command(OSPI_HandleTypeDef *handle,
                                   OSPI_RegularCmdTypeDef *command, uint32_t timeout)
{
    uint32_t index;
    (void)handle; (void)timeout;
    if (!powered) return HAL_ERROR;
    pending = *command;
    assert(command->InstructionMode == HAL_OSPI_INSTRUCTION_1_LINE);
    if (command->Instruction == 0x06u) write_enabled = 1;
    if (command->Instruction == 0x99u) write_enabled = 0;
    if (command->Instruction == 0xd8u) {
        assert(write_enabled);
        assert(command->Address == BASE || command->Address == BASE + SLOT);
        for (index = 0u; index < SLOT; ++index) {
            if (!mutate()) return HAL_ERROR;
            nor[command->Address - BASE + index] = 0xffu;
        }
        write_enabled = 0;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_OSPI_Receive(OSPI_HandleTypeDef *handle, uint8_t *data, uint32_t timeout)
{
    (void)handle; (void)timeout;
    if (!powered) return HAL_ERROR;
    if (read_failure) {read_failure = 0; return HAL_ERROR;}
    if (pending.Instruction == 0x9fu) {
        data[0] = 0xefu; data[1] = 0x40u; data[2] = 0x17u;
    } else if (pending.Instruction == 0x05u) {
        data[0] = write_enabled ? 2u : 0u;
    } else {
        assert(pending.Instruction == 0x03u);
        assert(pending.Address >= BASE && pending.Address + pending.NbData <= BASE + 2u * SLOT);
        memcpy(data, nor + pending.Address - BASE, pending.NbData);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_OSPI_Transmit(OSPI_HandleTypeDef *handle, uint8_t *data, uint32_t timeout)
{
    uint32_t index;
    (void)handle; (void)timeout;
    if (!powered) return HAL_ERROR;
    assert(write_enabled && pending.Instruction == 0x02u);
    assert(pending.Address >= BASE && pending.Address + pending.NbData <= BASE + 2u * SLOT);
    assert((pending.Address & 255u) + pending.NbData <= 256u);
    for (index = 0u; index < pending.NbData; ++index) {
        if (!mutate()) return HAL_ERROR;
        nor[pending.Address - BASE + index] &= data[index];
    }
    write_enabled = 0;
    return HAL_OK;
}

static void reboot(void)
{
    powered = 1;
    write_enabled = 0;
    cut_after = -1;
    assert(board_store_init());
}

static void expect_load(const uint8_t *expected, uint32_t count)
{
    static uint8_t actual[BOARD_STORE_CAPACITY];
    uint32_t length = 0u;
    assert(board_store_load(actual, sizeof(actual), &length));
    assert(length == count && memcmp(actual, expected, count) == 0);
}

static uint32_t crc32(const uint8_t *data, uint32_t length)
{
    uint32_t value = 0xffffffffu, index, bit;
    for (index = 0u; index < length; ++index) {
        value ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return value ^ 0xffffffffu;
}

int main(void)
{
    uint8_t old_data[257], new_data[513];
    static uint8_t maximum[BOARD_STORE_CAPACITY];
    uint32_t index, length;
    int cut;
    memset(nor, 0xff, sizeof(nor));
    for (index = 0u; index < sizeof(old_data); ++index) old_data[index] = (uint8_t)index;
    for (index = 0u; index < sizeof(new_data); ++index) new_data[index] = (uint8_t)(index * 7u);
    for (index = 0u; index < sizeof(maximum); ++index) maximum[index] = (uint8_t)(index * 11u);
    reboot();
    length = 99u;
    assert(!board_store_load(maximum, sizeof(maximum), &length) && length == 0u);
    assert(!board_store_save(NULL, 1u));
    assert(!board_store_save(maximum, 0u));
    assert(!board_store_save(maximum, sizeof(maximum) + 1u));
    assert(board_store_save(old_data, sizeof(old_data)));
    expect_load(old_data, sizeof(old_data));
    memcpy(baseline, nor, sizeof(nor));

    /* Representative interrupted erases plus every byte of header/payload/commit. */
    for (cut = 0; cut <= (int)(SLOT + 32u + sizeof(new_data) + 4u); ++cut) {
        if (cut > 1 && cut < (int)SLOT - 1 && cut != (int)SLOT / 2) continue;
        memcpy(nor, baseline, sizeof(nor));
        reboot();
        cut_after = cut;
        (void)board_store_save(new_data, sizeof(new_data));
        reboot();
        if (cut < (int)(SLOT + 32u + sizeof(new_data) + 4u))
            expect_load(old_data, sizeof(old_data));
        else
            expect_load(new_data, sizeof(new_data));
    }
    /* Corrupt the latest payload, header, or commit and recover the older slot. */
    for (index = 0u; index < 3u; ++index) {
        static const uint32_t corrupt_at[] = {32u, 8u, 24u};
        memcpy(nor, baseline, sizeof(nor));
        assert(board_store_save(new_data, sizeof(new_data)));
        nor[SLOT + corrupt_at[index]] ^= 1u;
        expect_load(old_data, sizeof(old_data));
    }
    memcpy(nor, baseline, sizeof(nor));
    read_failure = 1;
    assert(!board_store_save(new_data, sizeof(new_data)));
    assert(memcmp(nor, baseline, sizeof(nor)) == 0);
    assert(board_store_save(maximum, sizeof(maximum)));
    expect_load(maximum, sizeof(maximum));
    assert(!board_store_load(new_data, sizeof(new_data), &length) && length == 0u);

    /* Sequence 0 must supersede 0xffffffff after rollover. */
    memcpy(nor, baseline, sizeof(nor));
    memset(nor + 8u, 0xff, 4u);
    {
        uint32_t crc = crc32(nor, 20u);
        for (index = 0u; index < 4u; ++index) nor[20u + index] = (uint8_t)(crc >> (index * 8u));
    }
    assert(board_store_save(new_data, sizeof(new_data)));
    expect_load(new_data, sizeof(new_data));
    puts("board_store: power cuts, corruption fallback, read faults, size limits and rollover passed");
    return 0;
}
