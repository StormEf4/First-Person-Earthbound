#include "rom_check.h"

#include <stdio.h>
#include <string.h>

#define EB_ROM_SIZE 0x300000
#define HIROM_HEADER 0xFFC0
/* No-Intro "EarthBound (USA)". A mismatch is only a warning: it may be a re-dump or a hack. */
#define EB_US_CRC32 0xDC9BB451u

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

RomInfo rom_check(const uint8_t *data, size_t size)
{
    RomInfo r = {0};
    r.header_size = (size % 0x8000 == 512) ? 512 : 0;
    const uint8_t *rom = data + r.header_size;
    size_t rom_size = size - r.header_size;

    if (rom_size < HIROM_HEADER + 0x40) {
        snprintf(r.message, sizeof r.message, "That file is too small to be an SNES ROM.");
        return r;
    }
    memcpy(r.title, rom + HIROM_HEADER, 21);
    for (int i = 20; i >= 0 && r.title[i] == ' '; i--) r.title[i] = 0;

    if (strncmp(r.title, "MOTHER-2", 8) == 0) {
        snprintf(r.message, sizeof r.message,
                 "This is Mother 2 (the Japanese version).\nThis mod needs the US EarthBound ROM.");
        return r;
    }
    if (strcmp(r.title, "EARTH BOUND") != 0 || rom_size != EB_ROM_SIZE) {
        snprintf(r.message, sizeof r.message,
                 "This doesn't look like US EarthBound (title \"%s\", %zu bytes).", r.title, rom_size);
        return r;
    }
    r.crc32 = crc32(rom, rom_size);
    if (r.crc32 != EB_US_CRC32)
        snprintf(r.message, sizeof r.message,
                 "EarthBound ROM, but not the standard US dump (CRC32 %08X). It may be modified.",
                 r.crc32);
    r.ok = true;
    return r;
}
