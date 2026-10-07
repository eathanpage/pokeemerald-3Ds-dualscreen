/*
 * Game side of the BW sprites (3ds_bw.h): decompress.c calls PortBw_PicLoaded
 * with each picture it has just decompressed, and the Pokemon pictures among
 * them are handed to the compositor's side, frame by frame, with what it
 * needs to know about them (which BW sprite, the picture's palettes, where a
 * back picture meets the battle message box).
 */
#include "global.h"
#include "data.h"
#include "decompress.h"
#include "pokemon.h"
#include "constants/battle.h"
#include "constants/species.h"
#include "../compat/port_platform.h"
#include "3ds_bw.h"

#define PIC_FRAME_SIZE (64 * 64 / 2)
#define NUM_PIC_ENTRIES (SPECIES_UNOWN_QMARK + 1)

/* Which of the picture tables `sheet` is in (front or back), and where. */
static bool8 FindPic(const struct CompressedSpriteSheet *sheet, u16 *species, u8 *side)
{
    if (sheet >= gMonFrontPicTable && sheet < gMonFrontPicTable + NUM_PIC_ENTRIES)
    {
        *species = sheet - gMonFrontPicTable;
        *side = CTR_BW_SIDE_FRONT;
        return TRUE;
    }
    if (sheet >= gMonStillFrontPicTable && sheet < gMonStillFrontPicTable + NUM_PIC_ENTRIES)
    {
        *species = sheet - gMonStillFrontPicTable;
        *side = CTR_BW_SIDE_FRONT;
        return TRUE;
    }
    if (sheet >= gMonBackPicTable && sheet < gMonBackPicTable + NUM_PIC_ENTRIES)
    {
        *species = sheet - gMonBackPicTable;
        *side = CTR_BW_SIDE_BACK;
        return TRUE;
    }
    return FALSE;
}

static u32 UncompressedSize(const void *data)
{
    const u32 *payload = Port_ResolveAssetPointer(data);
    return payload ? payload[0] >> 8 : 0;
}

void PortBw_PicLoaded(const struct CompressedSpriteSheet *sheet, const void *dest)
{
    static u16 normalPal[16 * NUM_CASTFORM_FORMS], shinyPal[16 * NUM_CASTFORM_FORMS];
    static u16 palSpecies = 0xFFFF;
    static u32 numColors;
    u16 species, dex;
    u8 side, formBase = 0, numForms = 1;
    u32 size, numFrames, perForm, f;

    if (!sheet || !dest || !CtrBw_Init() || !FindPic(sheet, &species, &side))
        return;
    if (species == SPECIES_NONE || species == SPECIES_EGG)
        return;
    if (species >= SPECIES_UNOWN_B)
    {
        dex = SpeciesToNationalPokedexNum(SPECIES_UNOWN);
        formBase = species - SPECIES_UNOWN_B + 1;
    }
    else
    {
        dex = SpeciesToNationalPokedexNum(species);
    }
    if (dex == 0 || dex > 386)
        return;
    if (species == SPECIES_CASTFORM)
        numForms = NUM_CASTFORM_FORMS;

    size = UncompressedSize(sheet->data);
    numFrames = size / PIC_FRAME_SIZE;
    perForm = numFrames / numForms;
    if (!perForm)
        return;

    /* The picture's palettes, normal and shiny (Castform's hold every form). */
    if (palSpecies != species)
    {
        u32 numShiny;
        numColors = gMonPaletteTable[species].data && UncompressedSize(gMonPaletteTable[species].data) <= sizeof(normalPal)
                    ? UncompressedSize(gMonPaletteTable[species].data) / 2 : 0;
        numShiny = gMonShinyPaletteTable[species].data && UncompressedSize(gMonShinyPaletteTable[species].data) <= sizeof(shinyPal)
                   ? UncompressedSize(gMonShinyPaletteTable[species].data) / 2 : 0;
        if (numColors)
            LZ77UnCompWram(gMonPaletteTable[species].data, normalPal);
        if (numShiny)
            LZ77UnCompWram(gMonShinyPaletteTable[species].data, shinyPal);
        if (numShiny < numColors)
            numColors = numShiny;
        palSpecies = species;
    }
    if (numColors < 16)
        return;

    for (f = 0; f < numFrames && f / perForm < numForms; f++)
    {
        u8 form = f / perForm;
        u32 pal = numColors >= (form + 1) * 16u ? form * 16 : 0;

        CtrBw_RegisterFrame(dex, formBase + form, side, species, (const u8 *)dest + f * PIC_FRAME_SIZE,
                            normalPal + pal, shinyPal + pal, 64 - gMonBackPicCoords[species].y_offset);
    }
}
