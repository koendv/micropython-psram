#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "mpu.h"
#include "qspi.h"
#include "pin_static_af.h"
#include "psram.h"

#if defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)

// from ESP-PSRAM64H/LY68L6400SLIT datasheet
#define SRAM_CMD_READ           0x03
#define SRAM_CMD_FAST_READ      0x0b
#define SRAM_CMD_QUAD_READ      0xeb
#define SRAM_CMD_WRITE          0x02
#define SRAM_CMD_QUAD_WRITE     0x38
#define SRAM_CMD_QUAD_ON        0x35
#define SRAM_CMD_QUAD_OFF       0xf5
#define SRAM_CMD_RST_EN         0x66
#define SRAM_CMD_RST            0x99
#define SRAM_CMD_BURST_LEN      0xc0
#define SRAM_CMD_READ_ID        0x9f

#define QSPI_MAP_ADDR (0x90000000)

#ifndef MICROPY_HW_QSPI_PRESCALER
#define MICROPY_HW_QSPI_PRESCALER       3  // F_CLK = F_AHB/3 (72MHz when CPU is 216MHz)
#endif

#ifndef MICROPY_HW_QSPI_SAMPLE_SHIFT
#define MICROPY_HW_QSPI_SAMPLE_SHIFT    1  // sample shift enabled
#endif

#ifndef MICROPY_HW_QSPI_TIMEOUT_COUNTER
#define MICROPY_HW_QSPI_TIMEOUT_COUNTER 0  // timeout counter disabled (see F7 errata)
#endif

#ifndef MICROPY_HW_QSPI_CS_HIGH_CYCLES
#define MICROPY_HW_QSPI_CS_HIGH_CYCLES  2  // nCS stays high for 2 cycles
#endif

#if (MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2 - 3 - 1) >= 24
#define QSPI_CMD 0xec
#define QSPI_ADSIZE 3
#else
#define QSPI_CMD 0xeb
#define QSPI_ADSIZE 2
#endif

#define mp_raise_RuntimeError(msg) (mp_raise_msg(&mp_type_RuntimeError, (msg)))

static inline void qspi_mpu_disable_all(void) {
    // Configure MPU to disable access to entire QSPI region, to prevent CPU
    // speculative execution from accessing this region and modifying QSPI registers.
    uint32_t irq_state = mpu_config_start();
    mpu_config_region(MPU_REGION_QSPI1, QSPI_MAP_ADDR, MPU_CONFIG_DISABLE(0x00, MPU_REGION_SIZE_256MB));
    mpu_config_end(irq_state);
}

static inline void qspi_mpu_enable_mapped(void) {
    // Configure MPU to allow access to only the valid part of external SPI flash.
    // The memory accesses to the mapped QSPI are faster if the MPU is not used
    // for the memory-mapped region, so 3 MPU regions are used to disable access
    // to everything except the valid address space, using holes in the bottom
    // of the regions and nesting them.
    // At the moment this is hard-coded to 2MiB of QSPI address space.
    uint32_t irq_state = mpu_config_start();
    mpu_config_region(MPU_REGION_QSPI1, QSPI_MAP_ADDR, MPU_CONFIG_DISABLE(0x01, MPU_REGION_SIZE_256MB));
    mpu_config_region(MPU_REGION_QSPI2, QSPI_MAP_ADDR, MPU_CONFIG_DISABLE(0x0f, MPU_REGION_SIZE_32MB));
    mpu_config_region(MPU_REGION_QSPI3, QSPI_MAP_ADDR, MPU_CONFIG_DISABLE(0x01, MPU_REGION_SIZE_16MB));
    mpu_config_end(irq_state);
}

void qspi_init(void) {
    qspi_mpu_disable_all();

    // Configure pins
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_CS, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_BK1_NCS);
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_SCK, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_CLK);
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_IO0, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_BK1_IO0);
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_IO1, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_BK1_IO1);
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_IO2, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_BK1_IO2);
    mp_hal_pin_config_alt_static_speed(MICROPY_HW_QSPIRAM_IO3, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, MP_HAL_PIN_SPEED_VERY_HIGH, STATIC_AF_QUADSPI_BK1_IO3);

    // Bring up the QSPI peripheral

    __HAL_RCC_QSPI_CLK_ENABLE();
    __HAL_RCC_QSPI_FORCE_RESET();
    __HAL_RCC_QSPI_RELEASE_RESET();

    /*
       spi sram does internal refresh when NCS is high.
       keeping NCS low causes sram memory failure.
       to raise NCS after a read or write:
       - in QUADSPI->CR, set QUADSPI_CR_TCEN
       - set QUADSPI->LPTR to low value
     */

    QUADSPI->CR =
        (MICROPY_HW_QSPI_PRESCALER - 1) << QUADSPI_CR_PRESCALER_Pos
            | 3 << QUADSPI_CR_FTHRES_Pos // 4 bytes must be available to read/write
        #if defined(QUADSPI_CR_FSEL_Pos)
        | 0 << QUADSPI_CR_FSEL_Pos // FLASH 1 selected
        #endif
        #if defined(QUADSPI_CR_DFM_Pos)
        | 0 << QUADSPI_CR_DFM_Pos // dual-flash mode disabled
        #endif
        #if defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)
        | 1 << QUADSPI_CR_TCEN_Pos // raise NCS when mmapped sram not in use
        #endif
        | MICROPY_HW_QSPI_SAMPLE_SHIFT << QUADSPI_CR_SSHIFT_Pos
            | MICROPY_HW_QSPI_TIMEOUT_COUNTER << QUADSPI_CR_TCEN_Pos
            | 1 << QUADSPI_CR_EN_Pos // enable the peripheral
    ;

    QUADSPI->DCR =
        (MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2 - 3 - 1) << QUADSPI_DCR_FSIZE_Pos
            | (MICROPY_HW_QSPI_CS_HIGH_CYCLES - 1) << QUADSPI_DCR_CSHT_Pos
            | 0 << QUADSPI_DCR_CKMODE_Pos // CLK idles at low state
    ;

    QUADSPI->LPTR = 0; // raise NCS after 0 cycles of mmapped sram not in use

}

void qspi_memory_map(void) {
    // Enable memory-mapped mode

    QUADSPI->ABR = 0; // disable continuous read mode

    QUADSPI->CCR =
        0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
            | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
            | 3 << QUADSPI_CCR_FMODE_Pos // memory-mapped mode
            | 3 << QUADSPI_CCR_DMODE_Pos // data on 4 lines
            | 6 << QUADSPI_CCR_DCYC_Pos // 6 dummy cycles
            | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
            | QSPI_ADSIZE << QUADSPI_CCR_ADSIZE_Pos
            | 3 << QUADSPI_CCR_ADMODE_Pos // address on 4 lines
            | 3 << QUADSPI_CCR_IMODE_Pos // instruction on 4 lines
            | QSPI_CMD << QUADSPI_CCR_INSTRUCTION_Pos
    ;

    qspi_mpu_enable_mapped();
}

STATIC int qspi_ioctl(void *self_in, uint32_t cmd) {
    (void)self_in;
    switch (cmd) {
        case MP_QSPI_IOCTL_INIT:
            qspi_init();
            break;
        case MP_QSPI_IOCTL_BUS_ACQUIRE:
            // Disable memory-mapped region during bus access
            qspi_mpu_disable_all();
            // Abort any ongoing transfer if peripheral is busy
            if (QUADSPI->SR & QUADSPI_SR_BUSY) {
                QUADSPI->CR |= QUADSPI_CR_ABORT;
                while (QUADSPI->CR & QUADSPI_CR_ABORT) {
                }
            }
            break;
        case MP_QSPI_IOCTL_BUS_RELEASE:
            // Switch to memory-map mode when bus is idle
            qspi_memory_map();
            break;
    }
    return 0; // success
}


/* spi read id */

/* read id only works in spi mode, and when clock is <= 84 MHz */

STATIC uint32_t qspi_read_id() {
    const size_t len = 8;
    uint8_t cmd = SRAM_CMD_READ_ID;
    uint32_t spi_id[2];

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    QUADSPI->DLR = len - 1; // number of bytes to read

    QUADSPI->CCR =
        0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
            | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
            | 1 << QUADSPI_CCR_FMODE_Pos // indirect read mode
            | 1 << QUADSPI_CCR_DMODE_Pos // data on 1 line
            | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
            | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate bytes
            | 2 << QUADSPI_CCR_ADSIZE_Pos // 24 bit address size
            | 1 << QUADSPI_CCR_ADMODE_Pos // address on 1 lines
            | 1 << QUADSPI_CCR_IMODE_Pos // instruction on 1 line
            | cmd << QUADSPI_CCR_INSTRUCTION_Pos // read opcode
    ;

    QUADSPI->AR = 0;

    // Wait for read to finish
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
    }

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    // Read result
    spi_id[0] = QUADSPI->DR;
    spi_id[1] = QUADSPI->DR;

    // can only read spi id in spi mode, not in qspi mode
    if (!((spi_id[0] == 0) && (spi_id[1] == 0)) && !((spi_id[0] == ~0) && (spi_id[1] == ~0))) {
        mp_printf(MP_PYTHON_PRINTER, "psram eid: %04x %04x\n", spi_id[0], spi_id[1]);
    }

    return spi_id[0];
}


STATIC void qspi_write_cmd_data(void *self_in, uint8_t cmd, size_t len, uint32_t data) {
    (void)self_in;

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    if (len == 0) {
        QUADSPI->CCR =
            0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
                | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
                | 0 << QUADSPI_CCR_FMODE_Pos // indirect write mode
                | 0 << QUADSPI_CCR_DMODE_Pos // no data
                | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
                | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
                | 0 << QUADSPI_CCR_ADMODE_Pos // no address
                | 1 << QUADSPI_CCR_IMODE_Pos // instruction on 1 line
                | cmd << QUADSPI_CCR_INSTRUCTION_Pos // write opcode
        ;
    } else {
        QUADSPI->DLR = len - 1;

        QUADSPI->CCR =
            0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
                | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
                | 0 << QUADSPI_CCR_FMODE_Pos // indirect write mode
                | 1 << QUADSPI_CCR_DMODE_Pos // data on 1 line
                | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
                | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
                | 0 << QUADSPI_CCR_ADMODE_Pos // no address
                | 1 << QUADSPI_CCR_IMODE_Pos // instruction on 1 line
                | cmd << QUADSPI_CCR_INSTRUCTION_Pos // write opcode
        ;

        // This assumes len==2
        *(uint16_t *)&QUADSPI->DR = data;
    }

    // Wait for write to finish
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
    }

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag
}

STATIC void qspi_write_qcmd_qaddr_qdata(void *self_in, uint8_t cmd, uint32_t addr, size_t len, const uint8_t *src) {
    (void)self_in;

    uint8_t adsize = MP_SPI_ADDR_IS_32B(addr) ? 3 : 2;

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    if (len == 0) {
        QUADSPI->CCR =
            0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
                | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
                | 0 << QUADSPI_CCR_FMODE_Pos // indirect write mode
                | 0 << QUADSPI_CCR_DMODE_Pos // no data
                | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
                | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
                | adsize << QUADSPI_CCR_ADSIZE_Pos // 32/24-bit address size
                | 3 << QUADSPI_CCR_ADMODE_Pos // address on 4 lines
                | 3 << QUADSPI_CCR_IMODE_Pos // instruction on 4 lines
                | cmd << QUADSPI_CCR_INSTRUCTION_Pos // write opcode
        ;

        QUADSPI->AR = addr;
    } else {
        QUADSPI->DLR = len - 1;

        QUADSPI->CCR =
            0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
                | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
                | 0 << QUADSPI_CCR_FMODE_Pos // indirect write mode
                | 3 << QUADSPI_CCR_DMODE_Pos // data on 4 lines
                | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
                | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
                | adsize << QUADSPI_CCR_ADSIZE_Pos // 32/24-bit address size
                | 3 << QUADSPI_CCR_ADMODE_Pos // address on 4 lines
                | 3 << QUADSPI_CCR_IMODE_Pos // instruction on 4 lines
                | cmd << QUADSPI_CCR_INSTRUCTION_Pos // write opcode
        ;

        QUADSPI->AR = addr;

        // Write out the data 1 byte at a time
        while (len) {
            while (!(QUADSPI->SR & QUADSPI_SR_FTF)) {
            }
            *(volatile uint8_t *)&QUADSPI->DR = *src++;
            --len;
        }
    }

    // Wait for write to finish
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
    }

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag
}

STATIC uint32_t qspi_read_cmd(void *self_in, uint8_t cmd, size_t len) {
    (void)self_in;

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    QUADSPI->DLR = len - 1; // number of bytes to read

    QUADSPI->CCR =
        0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
            | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
            | 1 << QUADSPI_CCR_FMODE_Pos // indirect read mode
            | 1 << QUADSPI_CCR_DMODE_Pos // data on 1 line
            | 0 << QUADSPI_CCR_DCYC_Pos // 0 dummy cycles
            | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
            | 0 << QUADSPI_CCR_ADMODE_Pos // no address
            | 1 << QUADSPI_CCR_IMODE_Pos // instruction on 1 line
            | cmd << QUADSPI_CCR_INSTRUCTION_Pos // read opcode
    ;

    // Wait for read to finish
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
    }

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    // Read result
    return QUADSPI->DR;
}

STATIC void qspi_read_qcmd_qaddr_qdata(void *self_in, uint8_t cmd, uint32_t addr, size_t len, uint8_t *dest) {
    (void)self_in;

    uint8_t adsize = MP_SPI_ADDR_IS_32B(addr) ? 3 : 2;

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag

    QUADSPI->DLR = len - 1; // number of bytes to read

    QUADSPI->CCR =
        0 << QUADSPI_CCR_DDRM_Pos // DDR mode disabled
            | 0 << QUADSPI_CCR_SIOO_Pos // send instruction every transaction
            | 1 << QUADSPI_CCR_FMODE_Pos // indirect read mode
            | 3 << QUADSPI_CCR_DMODE_Pos // data on 4 lines
            | 6 << QUADSPI_CCR_DCYC_Pos // 6 dummy cycles
            | 0 << QUADSPI_CCR_ABMODE_Pos // no alternate byte
            | adsize << QUADSPI_CCR_ADSIZE_Pos // 32 or 24-bit address size
            | 3 << QUADSPI_CCR_ADMODE_Pos // address on 4 lines
            | 3 << QUADSPI_CCR_IMODE_Pos // instruction on 4 lines
            | cmd << QUADSPI_CCR_INSTRUCTION_Pos // quad read opcode
    ;

    QUADSPI->ABR = 0; // alternate byte: disable continuous read mode
    QUADSPI->AR = addr; // addres to read from

    // Read in the data 4 bytes at a time if dest is aligned
    if (((uintptr_t)dest & 3) == 0) {
        while (len >= 4) {
            while (!(QUADSPI->SR & QUADSPI_SR_FTF)) {
            }
            *(uint32_t *)dest = QUADSPI->DR;
            dest += 4;
            len -= 4;
        }
    }

    // Read in remaining data 1 byte at a time
    while (len) {
        while (!((QUADSPI->SR >> QUADSPI_SR_FLEVEL_Pos) & 0x3f)) {
        }
        *dest++ = *(volatile uint8_t *)&QUADSPI->DR;
        --len;
    }

    QUADSPI->FCR = QUADSPI_FCR_CTCF; // clear TC flag
}

/*
 This gets picked up by drivers/memory/spiflash.c
 */

const mp_qspi_proto_t qspi_proto = {
    .ioctl = qspi_ioctl,
    .write_cmd_data = qspi_write_cmd_data,
    .write_cmd_addr_data = qspi_write_qcmd_qaddr_qdata,
    .read_cmd = qspi_read_cmd,
    .read_cmd_qaddr_qdata = qspi_read_qcmd_qaddr_qdata,
};

// -----------------------------------------------------------------------------
// c interface for modules

void psram_init() {
    qspi_init();
    /* read id */
    qspi_read_id();
    /* set qspi mode */
    qspi_write_cmd_data(NULL, SRAM_CMD_QUAD_ON, 0, 0);
}

void psram_read(uint32_t addr, size_t len, uint8_t *dest) {
    qspi_read_qcmd_qaddr_qdata(NULL, SRAM_CMD_QUAD_READ, addr, len, (void *)dest);
}

void psram_write(uint32_t addr, size_t len, const uint8_t *src) {
    qspi_write_qcmd_qaddr_qdata(NULL, SRAM_CMD_QUAD_WRITE, addr, len, (void *)src);
}

#endif // defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)

