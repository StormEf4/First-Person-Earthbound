/* Identifies the user's ROM before we hand it to the emulator. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool ok;            /* US EarthBound, safe to run */
    size_t header_size; /* 512 if the file has a copier header, else 0 */
    uint32_t crc32;     /* of the headerless ROM */
    char title[22];     /* internal cartridge title */
    char message[256];  /* why it was rejected, or a warning */
} RomInfo;

RomInfo rom_check(const uint8_t *data, size_t size);
