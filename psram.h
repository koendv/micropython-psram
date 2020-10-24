#ifndef PSRAM_H
#define PSRAM_H

void psram_init();
void psram_read(uint32_t addr, size_t len, uint8_t *dest);
void psram_write(uint32_t addr, size_t len, const uint8_t *src);

#endif // defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)

