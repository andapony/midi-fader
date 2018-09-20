/**
 * LED Wristwatch Bootloader
 *
 * Kevin Cuzner
 */

#include "bootloader.h"

#include "stm32l0xx.h"
#include "nvm.h"
#include "usb_hid.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * Mask for RCC_CSR defining which bits may trigger an entry into bootloader mode:
 * - Any watchdog reset
 * - Any soft reset
 * - A pin reset (aka manual reset)
 * - A firewall reset
 */
#define BOOTLOADER_RCC_CSR_ENTRY_MASK (RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF | RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF | RCC_CSR_FWRSTF)

/**
 * Magic code value to make the bootloader ignore any of the entry bits set in
 * RCC_CSR and skip to the user program anyway, if a valid program start value
 * has been programmed.
 */
#define BOOTLOADER_MAGIC_SKIP 0x3C65A95A


static _EEPROM struct {
    uint32_t magic_code;
    union {
        uint32_t *user_vtor;
        uint32_t user_vtor_value;
    };
} bootloader_persistent_state;

typedef enum { ERR_NONE = 0, ERR_FSM = 1 << 0, ERR_COMMAND = 1 << 1, ERR_BAD_ADDR = 1 << 2, ERR_BAD_CRC32 = 1 << 3, ERR_WRITE = 1 << 4, ERR_SHORT = 1 << 5, ERR_VERIFY = 1 << 6 } BootloaderError;

#define CMD_RESET 0x00000000
#define CMD_PROG  0x00000080
#define CMD_READ  0x00000040
#define CMD_EXIT  0x000000C3
#define CMD_ABORT 0x0000003E

static union {
    uint32_t buffer[16];
    struct {
        uint32_t last_command;
        uint32_t flags;
        uint32_t crc32_lower;
        uint32_t crc32_upper;
        uint8_t data[48];
    };
} in_report;

union {
    uint32_t buffer[16];
    struct {
        uint32_t command;
        uint32_t *address;
        uint32_t crc32_lower;
        uint32_t crc32_upper;
    };
} out_report;

static const USBTransferData in_report_data = { &in_report, sizeof(in_report) };
static const USBTransferData out_report_data = { &out_report, sizeof(out_report) };

_Static_assert(sizeof(in_report) == 64, "Bootloader IN report is improperly sized. Fix this!");
_Static_assert(sizeof(out_report) == 64, "Bootloader OUT report is improperly sized. Fix this!");

typedef enum { EV_ANY, EV_CONFIGURED, EV_HID_OUT, EV_HID_IN, EV_HID_OUT_SHORT } BootloaderEvent;
typedef enum { ST_ANY, ST_RESET, ST_STATUS, ST_LPROG, ST_UPROG, ST_LREAD, ST_UREAD } BootloaderState;

typedef BootloaderState (*BootloaderFsmFn)(BootloaderEvent);

typedef struct {
    BootloaderState state;
    BootloaderEvent ev;
    BootloaderFsmFn fn;
} BootloaderFsmEntry;

static volatile struct {
    uint32_t *address;
    uint32_t crc32_lower;
    uint32_t crc32_upper;
    BootloaderState next_state; //used when sending a status report
} bootloader_state;

/**
 * Performs the sequence which will reset the device into the user program. Note
 * that if the EEPROM write performed during this function fails, it will erase
 * the user_vector_value to prevent booting into a bad program.
 */
static bool bootloader_reset_into_user_prog(void)
{
    //at this point, if a reset occurs we will still enter bootloader mode

    //write the magic bootloader-skip value so we can perform a reset
    if (!nvm_eeprom_write_w(&bootloader_persistent_state.magic_code, BOOTLOADER_MAGIC_SKIP))
    {
        nvm_eeprom_write_w(&bootloader_persistent_state.user_vtor_value, 0);
        return false;
    }

    //at this point, if a reset occurs we will not enter bootloader mode. We have entered the danger zone.

    //perform the system reset
    NVIC_SystemReset();

    //this shouldn't happen, but whatever
    return true;
}

/**
 * Sends a status report, starting an IN-OUT sequence
 */
static BootloaderState bootloader_send_status(BootloaderState next)
{
    bootloader_state.next_state = next;
    usb_hid_send(&in_report_data);

    return ST_STATUS;
}

/**
 * Sends a status report, queueing up a state change to ST_RESET
 */
static BootloaderState bootloader_enter_reset(void)
{
    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.last_command = CMD_RESET;
    return bootloader_send_status(ST_RESET);
}

/**
 * Sends a status report, queueing up a state change to ST_LPROG
 */
static BootloaderState bootloader_enter_prog(void)
{
    bootloader_state.address = out_report.address;
    bootloader_state.crc32_lower = out_report.crc32_lower;
    bootloader_state.crc32_upper = out_report.crc32_upper;

    //if needed, erase the previous entry point
    if (bootloader_persistent_state.user_vtor)
    {
        nvm_eeprom_write_w(&bootloader_persistent_state.user_vtor_value, 0);
    }

    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.last_command = CMD_PROG;

    uint32_t address = (uint32_t)bootloader_state.address;
    //determine if the address is aligned and in range
    if ((address & 0x7F) || (address < FLASH_LOWER_BOUND) || (address > FLASH_UPPER_BOUND))
    {
        in_report.flags = ERR_BAD_ADDR;
        return bootloader_send_status(ST_RESET);
    }
    else
    {
        //erase the page
        nvm_flash_erase_page(bootloader_state.address);
        return bootloader_send_status(ST_LPROG);
    }
}

/**
 * Sends a status report, queueing up a state change to ST_LREAD
 */
static BootloaderState bootloader_enter_read(void)
{
    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.last_command = CMD_READ;
    //TODO: CRC32
    //TODO: This can't use send_status because it creates an IN-IN-IN sequence
    return bootloader_send_status(ST_LREAD);
}

/**
 * Performs the bootloader exit sequence
 */
static BootloaderState bootloader_exit(void)
{
    BootloaderError error_flags = ERR_NONE;

    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.last_command = CMD_EXIT;
    //ensure we have somewhere to start the program
    if (!out_report.address)
    {
        error_flags = ERR_BAD_ADDR;
        goto error;
    }

    //write the passed address
    uint32_t address = (uint32_t)out_report.address;
    if ((address < FLASH_LOWER_BOUND) || (address > FLASH_UPPER_BOUND))
    {
        error_flags = ERR_BAD_ADDR;
        goto error;
    }
    if (!nvm_eeprom_write_w(&bootloader_persistent_state.user_vtor_value, address))
    {
        error_flags = ERR_WRITE;
        goto error;
    }

    //perform the system reset
    if (!bootloader_reset_into_user_prog())
    {
        error_flags = ERR_WRITE;
        goto error;
    }

    //if we somehow don't reset, return the bootloader to the reset state
    return ST_RESET;

error:
    in_report.flags = error_flags;
    return bootloader_send_status(ST_RESET);
}

static BootloaderState bootloader_abort(void)
{
    BootloaderError error_flags = ERR_NONE;
    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.last_command = CMD_ABORT;

    if (!bootloader_persistent_state.user_vtor_value)
    {
        error_flags = ERR_BAD_ADDR;
        goto error;
    }

    //perform the system reset
    if (!bootloader_reset_into_user_prog())
    {
        error_flags = ERR_WRITE;
        goto error;
    }

    //if we somehow don't reset, return the bootloader to the reset state
    return ST_RESET;

error:
    in_report.flags = error_flags;
    return bootloader_send_status(ST_RESET);
}

static BootloaderState bootloader_fsm_configured(BootloaderEvent ev)
{
    usb_hid_receive(&out_report_data);
    return ST_RESET;
}

static BootloaderState bootloader_fsm_reset(BootloaderEvent ev)
{
    switch (ev)
    {
    case EV_HID_OUT:
        GPIOB->BSRR = GPIO_BSRR_BR_7;
        if (out_report.command == CMD_RESET)
        {
            return bootloader_enter_reset();
        }
        else if (out_report.command == CMD_PROG)
        {
            return bootloader_enter_prog();
        }
        else if (out_report.command == CMD_READ)
        {
            bootloader_state.address = out_report.address;
            return bootloader_enter_read();
        }
        else if (out_report.command == CMD_EXIT)
        {
            return bootloader_exit();
        }
        else if (out_report.command == CMD_ABORT)
        {
            return bootloader_abort();
        }
        else
        {
            memset(in_report.buffer, 0, sizeof(in_report));
            in_report.last_command = out_report.command;
            in_report.flags = ERR_COMMAND;
            return bootloader_send_status(ST_RESET);
        }
        break;
    default:
        memset(in_report.buffer, 0, sizeof(in_report));
        in_report.flags = ERR_FSM;
        return bootloader_send_status(ST_RESET);
    }
}

static BootloaderState bootloader_fsm_status(BootloaderEvent ev)
{
    GPIOB->BSRR = GPIO_BSRR_BS_7;
    usb_hid_receive(&out_report_data);
    return bootloader_state.next_state;
}

/**
 * Shared function to writing an OUT report to flash
 */
static BootloaderState bootloader_fsm_program(bool upper, BootloaderEvent ev)
{
    uint32_t i, crc32, computed_crc32;
    uint32_t *address;
    BootloaderError error_flags = ERR_NONE;

    if (ev != EV_HID_OUT)
    {
        error_flags = ERR_FSM;
        goto error;
    }

    crc32 = upper ? bootloader_state.crc32_upper : bootloader_state.crc32_lower;

    //check the CRC32 (to avoid unexpected programming events)
    CRC->CR |= CRC_CR_RESET;
    volatile uint8_t *dr = &CRC->DR;
    for (i = 0; i < 16; i++)
    {
        CRC->DR = out_report.buffer[i];
    }
    computed_crc32 = ~CRC->DR; //invert result for zlib
    if (upper)
        in_report.crc32_upper = computed_crc32;
    else
        in_report.crc32_lower = computed_crc32;
    if (crc32 != computed_crc32)
    {
        error_flags = ERR_BAD_CRC32;
        goto error;
    }

    //program the page
    address = upper ? &bootloader_state.address[16] : bootloader_state.address;
    if (!nvm_flash_write_half_page(address, out_report.buffer))
    {
        error_flags = ERR_WRITE;
        goto error;
    }

    //verify the page
    for (i = 0; i < 16; i++)
    {
        if (address[i] != out_report.buffer[i])
        {
            error_flags = ERR_VERIFY;
            goto error;
        }
    }

    GPIOB->BSRR = GPIO_BSRR_BR_7;
    in_report.flags = ERR_NONE;
    return bootloader_send_status(upper ? ST_RESET : ST_UPROG);

error:
    in_report.flags = error_flags;
    return bootloader_send_status(ST_RESET);
}

/**
 * State executed when the lower page to program has been received
 */
static BootloaderState bootloader_fsm_lprog(BootloaderEvent ev)
{
    return bootloader_fsm_program(false, ev);
}

/**
 * State executed when the upper page to program has been received
 */
static BootloaderState bootloader_fsm_uprog(BootloaderEvent ev)
{
    return bootloader_fsm_program(true, ev);
}

static BootloaderState bootloader_fsm_short(BootloaderEvent ev)
{
    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.flags = ERR_SHORT;
    return bootloader_send_status(ST_RESET);
}

static BootloaderState bootloader_fsm_error(BootloaderEvent ev)
{
    memset(in_report.buffer, 0, sizeof(in_report));
    in_report.flags = ERR_FSM;
    return bootloader_send_status(ST_RESET);
}

static const BootloaderFsmEntry fsm[] = {
    { ST_ANY, EV_CONFIGURED, &bootloader_fsm_configured },
    { ST_RESET, EV_HID_OUT, &bootloader_fsm_reset },
    { ST_STATUS, EV_HID_IN, &bootloader_fsm_status },
    { ST_LPROG, EV_HID_OUT, &bootloader_fsm_lprog },
    { ST_UPROG, EV_HID_OUT, &bootloader_fsm_uprog },
    { ST_ANY, EV_HID_OUT_SHORT, &bootloader_fsm_short },
    { ST_ANY, EV_ANY, &bootloader_fsm_error }
};
#define FSM_SIZE sizeof(fsm)/(sizeof(BootloaderFsmEntry))

void bootloader_init(void)
{
    //if the prog_start field is set and there are no entry bits set in the CSR (or the magic code is programmed appropriate), start the user program
    if (bootloader_persistent_state.user_vtor &&
            (!(RCC->CSR & BOOTLOADER_RCC_CSR_ENTRY_MASK) || bootloader_persistent_state.magic_code == BOOTLOADER_MAGIC_SKIP))
    {
        if (bootloader_persistent_state.magic_code)
            nvm_eeprom_write_w(&bootloader_persistent_state.magic_code, 0);
        __disable_irq();
        uint32_t sp = bootloader_persistent_state.user_vtor[0];
        uint32_t pc = bootloader_persistent_state.user_vtor[1];
        SCB->VTOR = bootloader_persistent_state.user_vtor_value;
        __asm__ __volatile__("mov sp,%0\n\t"
                "bx %1\n\t"
                : /* no output */
                : "r" (sp), "r" (pc)
                : "sp");
        while (1) { }
    }
}

void bootloader_run(void)
{
    RCC->CSR |= RCC_CSR_RMVF;

    //Enable clocks for the bootloader
    RCC->IOPENR |= RCC_IOPENR_IOPAEN | RCC_IOPENR_IOPBEN;
    RCC->AHBENR |= RCC_AHBENR_CRCEN;
    GPIOA->MODER &= ~GPIO_MODER_MODE5_1;
    GPIOB->MODER &= ~GPIO_MODER_MODE7_1;
    GPIOA->BSRR = GPIO_BSRR_BS_5;
    GPIOB->BSRR = GPIO_BSRR_BS_7;

    //zlib configuration: Reverse 32-bit in, reverse out, default polynomial and init value
    //Note that the result will need to be inverted
    CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_IN_1 | CRC_CR_REV_OUT;
}

/**
 * Handles bootloader state changes initiated by external events, such as
 * receiving HID events
 */
static void bootloader_tick(BootloaderEvent ev)
{
    static BootloaderState state = ST_RESET;
    uint32_t i;
    for (i = 0; i < FSM_SIZE; i++)
    {
        const BootloaderFsmEntry *entry = &fsm[i];
        if (entry->state == state || entry->state == ST_ANY)
        {
            if (entry->ev == ev || entry->ev == EV_ANY)
            {
                state = entry->fn(ev);
                return;
            }
        }
    }
}

void hook_usb_hid_configured(void)
{
    bootloader_tick(EV_CONFIGURED);
}

void hook_usb_hid_in_report_sent(const USBTransferData *report)
{
    bootloader_tick(EV_HID_IN);
}

uint32_t last_length;
void hook_usb_hid_out_report_received(const USBTransferData *report)
{
    if (report->len == 64)
    {
        bootloader_tick(EV_HID_OUT);
    }
    else
    {
        bootloader_tick(EV_HID_OUT_SHORT);
    }
}

