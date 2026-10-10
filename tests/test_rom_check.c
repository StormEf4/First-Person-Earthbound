/* Checks ROM identification on synthetic images (no real game data). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_check.h"

static int failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint8_t *fake_rom(size_t header, size_t size, const char *title)
{
    uint8_t *p = calloc(1, header + size);
    memset(p + header + 0xFFC0, ' ', 21);
    memcpy(p + header + 0xFFC0, title, strlen(title));
    return p;
}

int main(void)
{
    uint8_t *eb = fake_rom(0, 0x300000, "EARTH BOUND");
    RomInfo r = rom_check(eb, 0x300000);
    CHECK(r.ok);
    CHECK(r.header_size == 0);
    CHECK(strcmp(r.title, "EARTH BOUND") == 0);
    CHECK(r.message[0] != 0); /* synthetic image: CRC differs, so a warning */

    uint8_t *headered = fake_rom(512, 0x300000, "EARTH BOUND");
    r = rom_check(headered, 512 + 0x300000);
    CHECK(r.ok);
    CHECK(r.header_size == 512);

    uint8_t *m2 = fake_rom(0, 0x300000, "MOTHER-2");
    r = rom_check(m2, 0x300000);
    CHECK(!r.ok);
    CHECK(strstr(r.message, "Mother 2") != NULL);

    uint8_t *other = fake_rom(0, 0x100000, "SUPER MARIOWORLD");
    r = rom_check(other, 0x100000);
    CHECK(!r.ok);

    uint8_t tiny[100] = {0};
    r = rom_check(tiny, sizeof tiny);
    CHECK(!r.ok);

    free(eb);
    free(headered);
    free(m2);
    free(other);
    printf(failures ? "%d failure(s)\n" : "rom_check: all tests passed\n", failures);
    return failures != 0;
}
