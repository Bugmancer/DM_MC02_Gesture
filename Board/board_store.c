#include "gesture_board.h"
#include "octospi.h"

#include <string.h>

#define SLOT_FIRST 0x007e0000u
#define SLOT_BYTES 0x00010000u
#define HEADER_BYTES 32u
#define HEADER_MAGIC 0x31424d47u
#define HEADER_VERSION 1u
#define HEADER_COMMIT 0x54494d43u
#define COMMAND_TIMEOUT_MS 20u
#define ERASE_TIMEOUT_MS 2500u

typedef struct {
    uint32_t address;
    uint32_t sequence;
    uint32_t length;
    uint32_t crc;
    int valid;
} StoreSlot;

static int flash_ready;

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t index;
    unsigned int bit;
    for (index = 0u; index < length; ++index) {
        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static int flash_command(uint8_t instruction, uint32_t address,
                         int has_address, uint32_t length)
{
    OSPI_RegularCmdTypeDef command = {0};
    command.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    command.FlashId = HAL_OSPI_FLASH_ID_1;
    command.Instruction = instruction;
    command.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    command.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    command.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
    command.Address = address;
    command.AddressMode = has_address ? HAL_OSPI_ADDRESS_1_LINE : HAL_OSPI_ADDRESS_NONE;
    command.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
    command.AddressDtrMode = HAL_OSPI_ADDRESS_DTR_DISABLE;
    command.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    command.AlternateBytesDtrMode = HAL_OSPI_ALTERNATE_BYTES_DTR_DISABLE;
    command.DataMode = length != 0u ? HAL_OSPI_DATA_1_LINE : HAL_OSPI_DATA_NONE;
    command.NbData = length;
    command.DataDtrMode = HAL_OSPI_DATA_DTR_DISABLE;
    command.DummyCycles = 0u;
    command.DQSMode = HAL_OSPI_DQS_DISABLE;
    command.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    if (HAL_OSPI_Command(&hospi2, &command, COMMAND_TIMEOUT_MS) == HAL_OK) return 1;
    (void)HAL_OSPI_Abort(&hospi2);
    return 0;
}

static int flash_receive(uint8_t *buffer)
{
    if (HAL_OSPI_Receive(&hospi2, buffer, COMMAND_TIMEOUT_MS) == HAL_OK) return 1;
    (void)HAL_OSPI_Abort(&hospi2);
    return 0;
}

static int flash_status(uint8_t *status)
{
    return flash_command(0x05u, 0u, 0, 1u) && flash_receive(status);
}

static int flash_wait(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    do {
        uint8_t status;
        if (!flash_status(&status)) return 0;
        if ((status & 1u) == 0u) return 1;
        HAL_Delay(1u);
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    return 0;
}

static int flash_write_enable(void)
{
    uint8_t status;
    return flash_wait(COMMAND_TIMEOUT_MS) && flash_command(0x06u, 0u, 0, 0u) &&
           flash_status(&status) && (status & 2u) != 0u;
}

static int flash_read(uint32_t address, uint8_t *buffer, uint32_t length)
{
    if (length == 0u) return 1;
    return flash_command(0x03u, address, 1, length) && flash_receive(buffer);
}

static int flash_program(uint32_t address, const uint8_t *data, uint32_t length)
{
    while (length != 0u) {
        uint32_t count = 256u - (address & 255u);
        if (count > length) count = length;
        if (!flash_write_enable() || !flash_command(0x02u, address, 1, count)) return 0;
        if (HAL_OSPI_Transmit(&hospi2, (uint8_t *)(void *)data, COMMAND_TIMEOUT_MS) != HAL_OK) {
            (void)HAL_OSPI_Abort(&hospi2);
            return 0;
        }
        if (!flash_wait(COMMAND_TIMEOUT_MS)) return 0;
        address += count;
        data += count;
        length -= count;
    }
    return 1;
}

static int flash_erase_slot(uint32_t address)
{
    if (address != SLOT_FIRST && address != SLOT_FIRST + SLOT_BYTES) return 0;
    return flash_write_enable() && flash_command(0xd8u, address, 1, 0u) &&
           flash_wait(ERASE_TIMEOUT_MS);
}

int board_store_init(void)
{
    uint8_t id[3];
    GPIO_InitTypeDef pins = {0};
    flash_ready = 0;
    /* In ordinary SPI, IO2/IO3 are /WP and /HOLD. Keep both physically high
       independently of the flash's previous Quad Enable setting. */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1 | GPIO_PIN_3, GPIO_PIN_SET);
    pins.Pin = GPIO_PIN_1 | GPIO_PIN_3;
    pins.Mode = GPIO_MODE_OUTPUT_PP;
    pins.Pull = GPIO_PULLUP;
    pins.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &pins);
    HAL_Delay(5u);
    if (!flash_command(0xabu, 0u, 0, 0u)) return 0;
    HAL_Delay(1u);
    if (!flash_wait(ERASE_TIMEOUT_MS) || !flash_command(0x66u, 0u, 0, 0u) ||
        !flash_command(0x99u, 0u, 0, 0u)) return 0;
    HAL_Delay(1u);
    if (!flash_command(0x9fu, 0u, 0, sizeof(id)) || !flash_receive(id)) return 0;
    if (id[0] != 0xefu || (id[1] != 0x40u && id[1] != 0x60u) || id[2] != 0x17u)
        return 0;
    flash_ready = 1;
    return 1;
}

static int flash_crc(uint32_t address, uint32_t length, uint32_t expected)
{
    uint8_t data[256];
    uint32_t crc = 0xffffffffu;
    while (length != 0u) {
        uint32_t count = length < sizeof(data) ? length : sizeof(data);
        if (!flash_read(address, data, count)) return -1;
        crc = crc_update(crc, data, count);
        address += count;
        length -= count;
    }
    return (crc ^ 0xffffffffu) == expected;
}

static int inspect_slot(uint32_t address, StoreSlot *slot)
{
    uint8_t header[HEADER_BYTES];
    int checked;
    memset(slot, 0, sizeof(*slot));
    slot->address = address;
    if (!flash_read(address, header, sizeof(header))) return 0;
    if (read_u32(header) != HEADER_MAGIC || read_u32(header + 4u) != HEADER_VERSION ||
        read_u32(header + 24u) != HEADER_COMMIT) return 1;
    if ((crc_update(0xffffffffu, header, 20u) ^ 0xffffffffu) != read_u32(header + 20u)) return 1;
    slot->sequence = read_u32(header + 8u);
    slot->length = read_u32(header + 12u);
    slot->crc = read_u32(header + 16u);
    if (slot->length > BOARD_STORE_CAPACITY || slot->length == 0u) return 1;
    checked = flash_crc(address + HEADER_BYTES, slot->length, slot->crc);
    if (checked < 0) return 0;
    slot->valid = checked;
    return 1;
}

static StoreSlot *newest_slot(StoreSlot *a, StoreSlot *b)
{
    if (!a->valid) return b->valid ? b : NULL;
    if (!b->valid) return a;
    /* Unsigned half-range ordering handles sequence rollover. */
    return (uint32_t)(b->sequence - a->sequence) < 0x80000000u ? b : a;
}

int board_store_load(void *buffer, uint32_t capacity, uint32_t *length)
{
    StoreSlot slots[2];
    StoreSlot *latest;
    if (length != NULL) *length = 0u;
    if (!flash_ready || buffer == NULL || length == NULL) return 0;
    if (!inspect_slot(SLOT_FIRST, &slots[0]) ||
        !inspect_slot(SLOT_FIRST + SLOT_BYTES, &slots[1])) return 0;
    latest = newest_slot(&slots[0], &slots[1]);
    if (latest == NULL || latest->length > capacity) return 0;
    if (!flash_read(latest->address + HEADER_BYTES, (uint8_t *)buffer, latest->length)) return 0;
    if ((crc_update(0xffffffffu, (const uint8_t *)buffer, latest->length) ^ 0xffffffffu) != latest->crc)
        return 0;
    *length = latest->length;
    return 1;
}

int board_store_save(const void *data, uint32_t length)
{
    StoreSlot slots[2], verify;
    StoreSlot *latest;
    uint8_t header[HEADER_BYTES];
    uint8_t commit[4];
    uint32_t target, sequence, payload_crc;
    if (!flash_ready || data == NULL || length == 0u || length > BOARD_STORE_CAPACITY) return 0;
    if (!inspect_slot(SLOT_FIRST, &slots[0]) ||
        !inspect_slot(SLOT_FIRST + SLOT_BYTES, &slots[1])) return 0;
    latest = newest_slot(&slots[0], &slots[1]);
    target = latest != NULL && latest->address == SLOT_FIRST ? SLOT_FIRST + SLOT_BYTES : SLOT_FIRST;
    sequence = latest != NULL ? latest->sequence + 1u : 1u;
    payload_crc = crc_update(0xffffffffu, (const uint8_t *)data, length) ^ 0xffffffffu;
    memset(header, 0xff, sizeof(header));
    write_u32(header, HEADER_MAGIC);
    write_u32(header + 4u, HEADER_VERSION);
    write_u32(header + 8u, sequence);
    write_u32(header + 12u, length);
    write_u32(header + 16u, payload_crc);
    write_u32(header + 20u, crc_update(0xffffffffu, header, 20u) ^ 0xffffffffu);
    if (!flash_erase_slot(target) || !flash_program(target, header, sizeof(header)) ||
        !flash_program(target + HEADER_BYTES, (const uint8_t *)data, length)) return 0;
    if (flash_crc(target + HEADER_BYTES, length, payload_crc) != 1) return 0;
    /* Commit is programmed last; the previous slot is never erased here. */
    write_u32(commit, HEADER_COMMIT);
    if (!flash_program(target + 24u, commit, sizeof(commit))) return 0;
    return inspect_slot(target, &verify) && verify.valid &&
           verify.sequence == sequence && verify.length == length;
}
