/* Developer tool: renders part of the EarthBound overworld from a ROM to a PNG, top-down, to
 * check the map decoder. Usage: ebmapdump rom.sfc out.png [tx ty tw th [scale_div]] */
#include <stdio.h>
#include <stdlib.h>

#include "raylib.h"

#include "eb_world.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom out.png [tx ty tw th [div]]\n", argv[0]);
        return 1;
    }
    int tx = 0, ty = 0, tw = EBW_MAP_W, th = EBW_MAP_H, div = 1;
    if (argc >= 7) {
        tx = atoi(argv[3]);
        ty = atoi(argv[4]);
        tw = atoi(argv[5]);
        th = atoi(argv[6]);
    }
    if (argc >= 8) div = atoi(argv[7]);

    int size;
    unsigned char *data = LoadFileData(argv[1], &size);
    if (!data) return 1;
    int header = (size % 0x8000 == 512) ? 512 : 0;
    char err[256];
    if (!ebw_load(data + header, (size_t)(size - header), err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    int w = tw * 32 / div, h = th * 32 / div;
    uint32_t *img = calloc((size_t)w * h, 4);
    uint32_t tile[32 * 32];
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int mx = tx + x, my = ty + y;
            ebw_render_tile(ebw_tileset_at(mx, my), ebw_palette_at(mx, my), ebw_tile_at(mx, my), false, tile, 32);
            for (int py = 0; py < 32 / div; py++)
                for (int px = 0; px < 32 / div; px++)
                    img[(size_t)(y * 32 / div + py) * w + x * 32 / div + px] = tile[py * div * 32 + px * div];
        }
    /* EBMAPDUMP_FG=1: tint foreground (priority) minitiles magenta. */
    if (getenv("EBMAPDUMP_FG") && div == 1)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (ebw_foreground_at(tx * 4 + x / 8, ty * 4 + y / 8)) {
                    uint32_t c = img[(size_t)y * w + x];
                    img[(size_t)y * w + x] = (((c & 0xFEFEFE) >> 1) | 0x800080) | 0xFF000000u;
                }
    Image out = {img, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    ExportImage(out, argv[2]);
    printf("wrote %dx%d\n", w, h);
    return 0;
}
