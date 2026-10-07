#ifndef CTR_BW_H
#define CTR_BW_H

/*
 * Pokemon Black/White animated sprites in place of the game's Pokemon
 * pictures (3ds_bw.c).
 *
 * The game keeps loading its own 64x64 pictures into OBJ VRAM. As it
 * decompresses one (decompress.c, through 3ds_bw_bridge.c), every frame of
 * it is recorded by its pixels; the compositor then draws a sprite whose
 * tiles match one as the matching BW sprite instead (CtrBw_Prepare): at its
 * own size, animating, with its feet and middle where the picture's were,
 * coloured as the game colours the picture (fades, flashes, shiny).
 *
 * The sprites come from bw_sprites.pack (the PC port's pack format, made by
 * the builder from the PokeEmerald-Native repository's converted sprites),
 * read from the SD card as they are needed. Without it nothing changes.
 *
 * Plain C: included by the game-side bridge and by the native compositor.
 */

#include <stdbool.h>
#include <stdint.h>

#define CTR_BW_PACK_PATH "sdmc:/3ds/emerald3ds/bw_sprites.pack"
#define CTR_BW_SIDE_FRONT 0
#define CTR_BW_SIDE_BACK 1

/* Opens the pack (once; false without one). */
bool CtrBw_Init(void);

/*
 * One frame of one of the game's pictures, as it was decompressed: `tiles`
 * is 64x64 4bpp in 8x8 tiles. `dex`/`form`/`side` name the BW sprite; the
 * palettes are the picture's own (16 colours each). `backBoxTop` is, for a
 * back picture, the row of its 64x64 box at the top of the battle message
 * box (64 - the species' y_offset). Cheap to call again for a known frame.
 */
void CtrBw_RegisterFrame(uint16_t dex, uint8_t form, uint8_t side, uint16_t species, const uint8_t *tiles,
                         const uint16_t *normalPal, const uint16_t *shinyPal, int backBoxTop);

/* A sprite to draw instead of an OAM entry's tiles. */
typedef struct
{
    const void *tex;  /* C3D_Tex */
    float u0, v0, u1, v1; /* texture coordinates of the image's corners */
    int left, top;    /* its top-left in the 64x64 box of the OAM entry (unflipped) */
    int width, height;
} CtrBwSprite;

/*
 * The BW sprite for OAM entry `index` (attributes `obj`), with OBJ tiles at
 * `objVram` and OBJ palettes at `objPalettes`, or false. `frame` counts
 * frames. Valid until the next call for the same entry.
 */
bool CtrBw_Prepare(unsigned index, const uint16_t *obj, bool mapping1d, const uint8_t *objVram,
                   const uint16_t *objPalettes, uint32_t frame, CtrBwSprite *out);

#endif
