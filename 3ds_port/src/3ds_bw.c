/*
 * Pokemon Black/White animated sprites in place of the game's Pokemon
 * pictures (see 3ds_bw.h). Native: owns the pack file, the decoded frames and
 * the textures the compositor draws.
 *
 * Pack layout (little endian, offsets from the start of the file):
 *   header:  'BWSP', u32 version (1), u32 entry count, u32 0
 *   entries: u16 dex, u8 form, u8 side (0 front, 1 back),
 *            u16 canvas w, u16 canvas h, u16 crop x, u16 crop y, u16 w, u16 h,
 *            u16 unique frames, u16 sequence length,
 *            u32 palettes offset (16 normal + 16 shiny RGB555 colours),
 *            u32 sequence offset (u16 unique frame, u16 duration in 1/100 s),
 *            u32 frames offset (u32 offset per unique frame)
 *   Frames are GBA LZ77 of 4bpp pixels (low nibble first, rows padded to a
 *   byte), each XORed with the previous unique frame. Each entry's blob
 *   (palettes, sequence, frame table, frames) runs up to the next entry's.
 *
 * Colours follow whatever the game does to the picture's palette: the palette
 * in use is fitted as a linear function of the picture's own, per channel,
 * and the same function is applied to the BW colours. The fit also tells
 * normal from shiny.
 */
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_bw.h"
#include "3ds_log.h"
#include "3ds_video.h"

#define PIC_FRAME_SIZE (64 * 64 / 2)
#define HASH_SLOTS 16384 /* power of two, a few times the frames hashed (x8 each) */
#define MAX_PICS 1024
#define NUM_INSTANCES 16
#define NUM_DECODED 8
#define NUM_TEXTURES 8

typedef struct
{
    uint16_t dex;
    uint8_t form, side;
    uint16_t canvasWidth, canvasHeight;
    uint16_t cropX, cropY, width, height;
    uint16_t numFrames, seqLength;
    uint32_t palettesOffset, seqOffset, framesOffset;
} PackEntry;

/* One of the game's pictures (species, form and side). */
typedef struct
{
    int entry;              /* index into sEntries */
    uint16_t normalPal[16], shinyPal[16];
    int16_t feetY, middleX; /* in the 64x64 picture: below its lowest pixel, middle of its columns */
    int16_t backBoxTop;
    int16_t restTop, restBottom; /* rows with pixels in the BW first frame (-1: not looked at) */
} MonPic;

/* An entry's frames, decoded: the 4bpp rows of each, and its timing. */
typedef struct
{
    int entry;
    bool hd;           /* from the upscaled pack */
    uint8_t *frames;   /* numFrames x frameBytes */
    uint16_t *seq;     /* (frame, duration) pairs */
    uint16_t palettes[32];
    uint32_t lastUsed;
} Decoded;

typedef struct
{
    uint16_t tileBase, pic;
    uint32_t start, lastSeen;
    bool used;
} Instance;

typedef struct
{
    C3D_Tex tex;
    unsigned size;
    bool upscaled;   /* at 1.5x rather than 1x */
    bool upscaledFrom; /* what its pixels were made from */
    unsigned oamIndex;
    int decoded, frame;
    uint16_t colors[16];
    uint32_t lastUsed;
    bool used;
} Texture;

static FILE *sPack;
static long sPackSize;
static PackEntry *sEntries;
static uint32_t sNumEntries;
static MonPic *sPics;
static int sNumPics;
static struct { uint64_t hash; uint16_t pic; uint8_t rows; bool used; } *sTable;
static struct { uint32_t key; int pic; } *sKnown; /* (dex, form, side) -> picture */
static int sNumKnown;
static Instance sInstances[NUM_INSTANCES];
static Decoded sDecoded[NUM_DECODED];
static Texture sTextures[NUM_TEXTURES];
static bool sShiny[16]; /* last normal/shiny decision per palette */
static bool sBgShiny[16]; /* the same for background palettes (CtrBw_PrepareBg) */
static uint32_t sFrame;
static bool sTried;
static bool sUpscale;
static uint8_t FramePixel(const PackEntry *e, const uint8_t *frame, int x, int y);

void CtrBw_SetUpscale(bool upscale)
{
    sUpscale = upscale;
}

/*
 * In the battle, drawn at 1.5 (3ds_video.c), a frame comes from the upscaled
 * pack instead: bw_sprites_hd.pack, every frame of the same entries upscaled
 * 3x ahead of time (scripts/make_hd_bw.c), averaged 2x2 here into 1.5x with
 * the colours of the moment. The entries' places and sizes stay the plain
 * pack's; the upscaled pack only gives the pictures.
 */
static FILE *sPackHd;
static long sPackHdSize;
static PackEntry *sEntriesHd;

static void AverageFrame(const PackEntry *e, const uint8_t *pixels, const uint16_t *colors, uint16_t *data,
                         unsigned size)
{
    int w = e->width, h = e->height, ow = (w + 1) / 2, oh = (h + 1) / 2;

    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
        {
            unsigned r = 0, g = 0, b = 0, n = 0;

            for (int k = 0; k < 4; k++)
            {
                int bx = x * 2 + k % 2, by = y * 2 + k / 2;
                uint8_t c;

                if (bx >= w || by >= h || !(c = FramePixel(e, pixels, bx, by))) continue;
                r += colors[c] & 31;
                g += (colors[c] >> 5) & 31;
                b += (colors[c] >> 10) & 31;
                n++;
            }
            /* RGBA5551: opaque where most of the texel is covered. */
            data[CtrVideo_Texel((unsigned)x, (unsigned)y, size)] =
                n >= 2 ? (uint16_t)(((r + n / 2) / n) << 11 | ((g + n / 2) / n) << 6 | ((b + n / 2) / n) << 1 | 1) : 0;
        }
}

/* ── Pack ────────────────────────────────────────────────────────────────── */

bool CtrBw_Init(void)
{
    uint32_t header[4];

    if (sTried)
        return sPack != NULL;
    sTried = true;
    sPack = fopen(CTR_BW_PACK_PATH, "rb");
    if (!sPack)
        sPack = fopen("romfs:/bw/bw_sprites.pack", "rb");
    if (!sPack)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "bw: no sprite pack, the game's pictures are shown");
        return false;
    }
    fseek(sPack, 0, SEEK_END);
    sPackSize = ftell(sPack);
    fseek(sPack, 0, SEEK_SET);
    if (fread(header, 1, sizeof(header), sPack) != sizeof(header) || memcmp(header, "BWSP", 4) || header[1] != 1
        || header[2] == 0 || header[2] > 4096)
        goto fail;
    sNumEntries = header[2];
    /* The upscaled pictures, if there: the same entries, version 2. */
    sPackHd = fopen(CTR_BW_HD_PACK_PATH, "rb");
    if (sPackHd)
    {
        uint32_t hd[4];

        fseek(sPackHd, 0, SEEK_END);
        sPackHdSize = ftell(sPackHd);
        fseek(sPackHd, 0, SEEK_SET);
        sEntriesHd = malloc(sNumEntries * sizeof(PackEntry));
        if (fread(hd, 1, sizeof(hd), sPackHd) != sizeof(hd) || memcmp(hd, "BWSP", 4) || hd[1] != 2
            || hd[2] != sNumEntries || !sEntriesHd
            || fread(sEntriesHd, sizeof(PackEntry), sNumEntries, sPackHd) != sNumEntries)
        {
            CtrLog_Write(CTR_LOG_ERROR, "bw: bw_sprites_hd.pack does not match bw_sprites.pack, not used");
            fclose(sPackHd);
            sPackHd = NULL;
            free(sEntriesHd);
            sEntriesHd = NULL;
        }
        else
            CtrLog_Write(CTR_LOG_VIDEO, "bw: upscaled pictures for the battle (%ld bytes)", sPackHdSize);
    }
    sEntries = malloc(sNumEntries * sizeof(PackEntry));
    sPics = calloc(MAX_PICS, sizeof(MonPic));
    sTable = calloc(HASH_SLOTS, sizeof(*sTable));
    sKnown = calloc(MAX_PICS, sizeof(*sKnown));
    if (!sEntries || !sPics || !sTable || !sKnown
        || fread(sEntries, sizeof(PackEntry), sNumEntries, sPack) != sNumEntries)
        goto fail;
    CtrLog_Write(CTR_LOG_VIDEO, "bw: %lu sprites in the pack (%ld bytes)", (unsigned long)sNumEntries, sPackSize);
    return true;

fail:
    CtrLog_Write(CTR_LOG_ERROR, "bw: unreadable sprite pack");
    fclose(sPack);
    sPack = NULL;
    free(sEntries);
    free(sPics);
    free(sTable);
    free(sKnown);
    sEntries = NULL;
    sPics = NULL;
    sTable = NULL;
    sKnown = NULL;
    return false;
}

static int FindEntry(uint16_t dex, uint8_t form, uint8_t side)
{
    uint32_t lo = 0, hi = sNumEntries, key = ((uint32_t)dex << 16) | (form << 8) | side;

    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        uint32_t k = ((uint32_t)sEntries[mid].dex << 16) | (sEntries[mid].form << 8) | sEntries[mid].side;
        if (k == key)
            return (int)mid;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static bool ReadFrom(FILE *pack, uint32_t offset, void *dest, uint32_t size)
{
    return fseek(pack, (long)offset, SEEK_SET) == 0 && fread(dest, 1, size, pack) == size;
}

/* GBA LZ77 (type 0x10); false if it doesn't make exactly `size` bytes. */
static bool Lz77(const uint8_t *src, const uint8_t *end, uint8_t *dest, uint32_t size)
{
    uint32_t total, out = 0;

    if (end - src < 4 || src[0] != 0x10)
        return false;
    total = src[1] | (src[2] << 8) | (src[3] << 16);
    if (total != size)
        return false;
    src += 4;
    while (out < total && src < end)
    {
        uint8_t flags = *src++;
        for (int bit = 0; bit < 8 && out < total; bit++, flags <<= 1)
        {
            if (!(flags & 0x80))
            {
                if (src >= end)
                    return false;
                dest[out++] = *src++;
            }
            else
            {
                uint32_t length, disp;
                if (end - src < 2)
                    return false;
                length = (src[0] >> 4) + 3;
                disp = (((src[0] & 15) << 8) | src[1]) + 1;
                src += 2;
                if (disp > out)
                    return false;
                while (length-- && out < total)
                {
                    dest[out] = dest[out - disp];
                    out++;
                }
            }
        }
    }
    return out == total;
}

/* The entry's decoded frames (decoding them, if need be, over the least
 * recently used). */
static Decoded *DecodeFrom(int entry, bool hd);

static Decoded *Decode(int entry)
{
    return DecodeFrom(entry, false);
}

static Decoded *DecodeFrom(int entry, bool hd)
{
    const PackEntry *entries = hd ? sEntriesHd : sEntries;
    const PackEntry *e = &entries[entry];
    uint32_t frameBytes = (e->width + 1) / 2 * e->height;
    uint32_t blobEnd = entry + 1 < (int)sNumEntries ? entries[entry + 1].palettesOffset
                                                    : (uint32_t)(hd ? sPackHdSize : sPackSize);
    Decoded *slot = NULL;
    uint32_t *offsets = NULL;
    uint8_t *blob = NULL;
    int i;

    for (i = 0; i < NUM_DECODED; i++)
    {
        if (sDecoded[i].frames && sDecoded[i].entry == entry && sDecoded[i].hd == hd)
        {
            sDecoded[i].lastUsed = sFrame;
            return &sDecoded[i];
        }
        if (!slot || !sDecoded[i].frames || (slot->frames && sDecoded[i].lastUsed < slot->lastUsed))
            slot = &sDecoded[i];
    }
    /* Textures showing what's thrown out go with it. */
    for (i = 0; i < NUM_TEXTURES; i++)
        if (sTextures[i].used && sTextures[i].decoded == (int)(slot - sDecoded))
            sTextures[i].decoded = -1;
    free(slot->frames);
    free(slot->seq);
    memset(slot, 0, sizeof(*slot));

    if (blobEnd <= e->palettesOffset || blobEnd - e->palettesOffset > (hd ? 32u : 4u) << 20)
        return NULL;
    blob = malloc(blobEnd - e->palettesOffset);
    slot->frames = malloc(frameBytes * e->numFrames);
    slot->seq = malloc(e->seqLength * 4);
    if (!blob || !slot->frames || !slot->seq
        || !ReadFrom(hd ? sPackHd : sPack, e->palettesOffset, blob, blobEnd - e->palettesOffset))
        goto fail;
    memcpy(slot->palettes, blob, 64);
    memcpy(slot->seq, blob + (e->seqOffset - e->palettesOffset), e->seqLength * 4);
    offsets = (uint32_t *)(blob + (e->framesOffset - e->palettesOffset));
    for (i = 0; i < e->numFrames; i++)
    {
        uint8_t *frame = slot->frames + i * frameBytes;
        uint32_t start = offsets[i] - e->palettesOffset;

        if (offsets[i] < e->palettesOffset || start >= blobEnd - e->palettesOffset
            || !Lz77(blob + start, blob + (blobEnd - e->palettesOffset), frame, frameBytes))
            memset(frame, 0, frameBytes);
        /* Each frame is stored as the difference from the one before. */
        if (i > 0)
            for (uint32_t b = 0; b < frameBytes; b++)
                frame[b] ^= (frame - frameBytes)[b];
    }
    free(blob);
    slot->entry = entry;
    slot->hd = hd;
    slot->lastUsed = sFrame;
    return slot;

fail:
    free(blob);
    free(slot->frames);
    free(slot->seq);
    memset(slot, 0, sizeof(*slot));
    return NULL;
}

static uint8_t FramePixel(const PackEntry *e, const uint8_t *frame, int x, int y)
{
    uint8_t b = frame[y * ((e->width + 1) / 2) + (x >> 1)];
    return (x & 1) ? b >> 4 : b & 15;
}

/* ── The game's pictures ─────────────────────────────────────────────────── */

static uint64_t HashPic(const uint8_t *tiles)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < PIC_FRAME_SIZE; i++)
        h = (h ^ tiles[i]) * 0x100000001B3ull;
    return h;
}

static void InsertHash(uint64_t hash, uint16_t pic, uint8_t rows)
{
    uint32_t slot = (uint32_t)hash & (HASH_SLOTS - 1);
    for (int n = 0; sTable[slot].used; n++)
    {
        if (sTable[slot].hash == hash || n == HASH_SLOTS)
            return; /* already known (the first stays), or full */
        slot = (slot + 1) & (HASH_SLOTS - 1);
    }
    sTable[slot].hash = hash;
    sTable[slot].pic = pic;
    sTable[slot].rows = rows;
    sTable[slot].used = true;
}

static int LookupHash(uint64_t hash, uint8_t *rows)
{
    uint32_t slot = (uint32_t)hash & (HASH_SLOTS - 1);
    for (int n = 0; sTable[slot].used && n < HASH_SLOTS; n++)
    {
        if (sTable[slot].hash == hash)
        {
            *rows = sTable[slot].rows;
            return sTable[slot].pic;
        }
        slot = (slot + 1) & (HASH_SLOTS - 1);
    }
    return -1;
}

static uint8_t PicPixel(const uint8_t *tiles, int x, int y)
{
    uint8_t b = tiles[((y >> 3) * 8 + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    return (x & 1) ? b >> 4 : b & 15;
}

void CtrBw_RegisterFrame(uint16_t dex, uint8_t form, uint8_t side, uint16_t species, const uint8_t *tiles,
                         const uint16_t *normalPal, const uint16_t *shinyPal, int backBoxTop)
{
    static uint8_t erased[PIC_FRAME_SIZE];
    uint32_t key = ((uint32_t)dex << 16) | (form << 8) | side;
    uint64_t hash;
    uint8_t rows;
    int pic = -1, i;

    (void)species;
    if (!CtrBw_Init() || !tiles)
        return;
    hash = HashPic(tiles);
    if (LookupHash(hash, &rows) >= 0)
        return; /* known frame */

    for (i = 0; i < sNumKnown; i++)
        if (sKnown[i].key == key)
            pic = sKnown[i].pic;
    if (pic < 0)
    {
        MonPic *p;
        int entry = FindEntry(dex, form, side), x, y, minX = 64, maxX = -1, maxY = -1;

        if (entry < 0 || sNumPics >= MAX_PICS)
            return;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                if (PicPixel(tiles, x, y))
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    maxY = y;
                }
        if (maxY < 0)
            return; /* an empty frame tells nothing */
        p = &sPics[sNumPics];
        p->entry = entry;
        memcpy(p->normalPal, normalPal, 32);
        memcpy(p->shinyPal, shinyPal, 32);
        p->feetY = maxY + 1;
        p->middleX = (minX + maxX + 1) / 2;
        p->backBoxTop = backBoxTop;
        p->restTop = -1;
        pic = sNumPics++;
        sKnown[sNumKnown].key = key;
        sKnown[sNumKnown].pic = pic;
        sNumKnown++;
    }

    /* The frame, and the same with its bottom tile rows erased: a fainting
     * opponent sinks out of view that way, a row at a time. */
    InsertHash(hash, pic, 8);
    memcpy(erased, tiles, PIC_FRAME_SIZE);
    for (int r = 7; r >= 1; r--)
    {
        memset(erased + r * 8 * 32, 0, (8 - r) * 8 * 32);
        InsertHash(HashPic(erased), pic, r);
    }
}

/* ── Per frame ───────────────────────────────────────────────────────────── */

/* The frame to show `elapsed` game frames after the animation started. */
static int CurrentFrame(const PackEntry *e, const Decoded *d, uint32_t elapsed)
{
    uint32_t total = 0, t;
    int i;

    for (i = 0; i < e->seqLength; i++)
        total += d->seq[i * 2 + 1];
    if (!total)
        return 0;
    t = (uint32_t)((uint64_t)elapsed * 100 * 10000 / 597275) % total; /* 1/100 s at 59.7275 fps */
    for (i = 0; i < e->seqLength; i++)
    {
        if (t < d->seq[i * 2 + 1])
            return d->seq[i * 2] < e->numFrames ? d->seq[i * 2] : 0;
        t -= d->seq[i * 2 + 1];
    }
    return 0;
}

static uint32_t StartFrameFor(uint16_t tileBase, uint16_t pic)
{
    Instance *slot = NULL, *oldest = NULL;

    for (int i = 0; i < NUM_INSTANCES; i++)
    {
        Instance *in = &sInstances[i];
        /* Gone for a while: the next appearance starts the animation over. */
        if (in->used && in->lastSeen + 2 < sFrame)
            in->used = false;
        if (in->used && in->tileBase == tileBase)
        {
            /* A different Pokemon (Transform, a new one sent out): start over. */
            if (sPics[in->pic].entry != sPics[pic].entry)
                in->start = sFrame;
            in->pic = pic;
            in->lastSeen = sFrame;
            return in->start;
        }
        if (!in->used && !slot)
            slot = in;
        if (!oldest || in->lastSeen < oldest->lastSeen)
            oldest = in;
    }
    if (!slot)
        slot = oldest;
    slot->used = true;
    slot->tileBase = tileBase;
    slot->pic = pic;
    slot->start = sFrame;
    slot->lastSeen = sFrame;
    return slot->start;
}

/* Fits cur[i] = a * base[i] + b per channel over colours 1-15; the squared error. */
static float FitPalette(const uint16_t *base, const uint16_t *cur, float a[3], float b[3])
{
    float err = 0;

    for (int c = 0; c < 3; c++)
    {
        float mx = 0, my = 0, sxx = 0, sxy = 0;
        for (int i = 1; i < 16; i++)
        {
            mx += (base[i] >> (c * 5)) & 31;
            my += (cur[i] >> (c * 5)) & 31;
        }
        mx /= 15;
        my /= 15;
        for (int i = 1; i < 16; i++)
        {
            float x = (float)((base[i] >> (c * 5)) & 31) - mx;
            float y = (float)((cur[i] >> (c * 5)) & 31) - my;
            sxx += x * x;
            sxy += x * y;
        }
        a[c] = sxx > 0 ? sxy / sxx : 1;
        b[c] = my - a[c] * mx;
        for (int i = 1; i < 16; i++)
        {
            float x = (base[i] >> (c * 5)) & 31, y = (cur[i] >> (c * 5)) & 31;
            float d = y - (a[c] * x + b[c]);
            err += d * d;
        }
    }
    return err;
}

/* The BW colours for a picture shown with palette `cur`. */
static void MakeColors(const MonPic *p, const uint16_t *bwPal, const uint16_t *cur, bool *lastShiny, uint16_t *out)
{
    float an[3], bn[3], as[3], bs[3], *a, *b;
    bool shiny;

    if (!memcmp(cur + 1, p->normalPal + 1, 30))
    {
        *lastShiny = false;
        memcpy(out, bwPal, 32);
        return;
    }
    if (!memcmp(cur + 1, p->shinyPal + 1, 30))
    {
        *lastShiny = true;
        memcpy(out, bwPal + 16, 32);
        return;
    }
    float errNormal = FitPalette(p->normalPal, cur, an, bn);
    float errShiny = FitPalette(p->shinyPal, cur, as, bs);
    if (errNormal + 0.5f < errShiny)
        shiny = false;
    else if (errShiny + 0.5f < errNormal)
        shiny = true;
    else
        shiny = *lastShiny; /* e.g. faded to one colour: can't tell */
    *lastShiny = shiny;
    a = shiny ? as : an;
    b = shiny ? bs : bn;
    if (shiny)
        bwPal += 16;
    out[0] = 0;
    for (int i = 1; i < 16; i++)
    {
        uint16_t color = 0;
        for (int c = 0; c < 3; c++)
        {
            int v = (int)(a[c] * ((bwPal[i] >> (c * 5)) & 31) + b[c] + 0.5f);
            if (v < 0) v = 0;
            if (v > 31) v = 31;
            color |= v << (c * 5);
        }
        out[i] = color;
    }
}

/* Where the Pokemon is in the BW sprite's first frame (its standing pose). */
static void FindRestPose(MonPic *p, const Decoded *d)
{
    const PackEntry *e = &sEntries[p->entry];
    int x, y;

    p->restTop = 0;
    p->restBottom = e->height - 1;
    for (y = 0; y < e->height; y++)
    {
        for (x = 0; x < e->width && !FramePixel(e, d->frames, x, y); x++)
            ;
        if (x < e->width)
        {
            p->restTop = y;
            break;
        }
    }
    for (y = e->height - 1; y >= 0; y--)
    {
        for (x = 0; x < e->width && !FramePixel(e, d->frames, x, y); x++)
            ;
        if (x < e->width)
        {
            p->restBottom = y;
            break;
        }
    }
}

/* The texture showing frame `frame` of `d` in `colors` for OAM entry `index`. */
static Texture *TextureFor(unsigned index, Decoded *d, int frame, const uint16_t *colors)
{
    const PackEntry *e = &sEntries[d->entry];
    int decoded = (int)(d - sDecoded);
    unsigned size = 64;
    Texture *t = NULL;
    int i;
    /* In the battle, the upscaled frame: 1.5 texels a pixel. */
    Decoded *hd = sUpscale && sPackHd ? DecodeFrom(d->entry, true) : NULL;
    const PackEntry *eh = hd ? &sEntriesHd[d->entry] : NULL;
    int texelsW = hd ? (eh->width + 1) / 2 : e->width, texelsH = hd ? (eh->height + 1) / 2 : e->height;
    bool upscaled = hd != NULL;

    while (size < (unsigned)texelsW || size < (unsigned)texelsH)
        size *= 2;
    for (i = 0; i < NUM_TEXTURES; i++)
        if (sTextures[i].used && sTextures[i].oamIndex == index)
            t = &sTextures[i];
    if (!t)
    {
        for (i = 0; i < NUM_TEXTURES; i++)
            if (!t || !sTextures[i].used || (t->used && sTextures[i].lastUsed < t->lastUsed))
                t = &sTextures[i];
        t->used = true;
        t->oamIndex = index;
        t->decoded = -1;
    }
    t->lastUsed = sFrame;
    if (t->size != size || t->upscaled != upscaled)
    {
        if (t->tex.data)
            C3D_TexDelete(&t->tex);
        memset(&t->tex, 0, sizeof(t->tex));
        t->size = 0;
        if (!C3D_TexInit(&t->tex, size, size, GPU_RGBA5551))
        {
            memset(&t->tex, 0, sizeof(t->tex));
            t->used = false;
            return NULL;
        }
        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        memset(t->tex.data, 0, size * size * 2);
        t->size = size;
        t->upscaled = upscaled;
        t->decoded = -1;
    }
    if (t->decoded != decoded || t->frame != frame || memcmp(t->colors, colors, 32) || t->upscaledFrom != upscaled)
    {
        const uint8_t *pixels = d->frames + (uint32_t)frame * ((e->width + 1) / 2) * e->height;

        if (t->upscaled)
            AverageFrame(eh, hd->frames + (uint32_t)frame * ((eh->width + 1) / 2) * eh->height, colors, t->tex.data,
                         size);
        else
        {
            uint16_t *data = t->tex.data, rgba[16];

            rgba[0] = 0;
            for (i = 1; i < 16; i++)
                rgba[i] = CtrVideo_RGBA5551(colors[i]);
            for (int y = 0; y < e->height; y++)
                for (int x = 0; x < e->width; x++)
                    data[CtrVideo_Texel(x, y, size)] = rgba[FramePixel(e, pixels, x, y)];
        }
        C3D_TexFlush(&t->tex);
        t->decoded = decoded;
        t->frame = frame;
        t->upscaledFrom = upscaled;
        memcpy(t->colors, colors, 32);
    }
    return t;
}

/* The pictures OAM entries showed this frame, by their first tile. */
static struct { uint16_t tileBase; int16_t pic; uint8_t rows; } sSeen[16];
static int sNumSeen;

static void NewFrame(uint32_t frame)
{
    if (frame != sFrame)
    {
        sFrame = frame;
        sNumSeen = 0;
    }
}

/* The texture and placement of picture `pic`'s BW sprite, animating from
 * `start`, coloured from `palette` (the 16 colours the picture is shown in). */
static bool FinishSprite(unsigned texIndex, int pic, uint8_t rows, uint32_t start, const uint16_t *palette,
                         bool *shiny, CtrBwSprite *out)
{
    MonPic *p = &sPics[pic];
    const PackEntry *e = &sEntries[p->entry];
    Decoded *d = Decode(p->entry);
    uint16_t colors[16];
    Texture *t;
    int k;

    if (!d)
        return false;
    k = CurrentFrame(e, d, sFrame - start);
    MakeColors(p, d->palettes, palette, shiny, colors);
    t = TextureFor(texIndex, d, k, colors);
    if (!t)
        return false;

    out->tex = &t->tex;
    out->width = e->width;
    out->height = e->height;
    /* Feet and middle where the picture's are. */
    out->left = p->middleX - e->width / 2;
    out->top = p->feetY - e->height;
    /* BW shows the player's Pokemon lower, the bottom of it out of view behind
     * the message box: the bottom quarter of its standing pose goes below the
     * box's top edge. */
    if (e->side == CTR_BW_SIDE_BACK)
    {
        if (p->restTop < 0)
            FindRestPose(p, d);
        out->top = p->backBoxTop + (p->restBottom - p->restTop + 1) / 4 - (p->restBottom + 1);
    }
    /* Erased from the bottom (fainting): hidden below the same line. */
    if (rows < 8 && out->top + out->height > rows * 8)
        out->height = rows * 8 - out->top;
    if (out->height <= 0)
        return false;
    out->scale = t->upscaled ? 1.5f : 1.0f;
    out->u0 = 0;
    out->v0 = 1.0f;
    out->u1 = e->width * out->scale / (float)t->size;
    out->v1 = 1.0f - out->height * out->scale / (float)t->size;
    return true;
}

bool CtrBw_PrepareBg(unsigned bg, const uint8_t *tiles, const uint16_t *palette, unsigned paletteSlot,
                     uint32_t frame, CtrBwSprite *out)
{
    uint8_t rows = 8;
    uint32_t start = 0;
    int pic;
    bool found = false;

    if (!sPack || !sNumPics || bg > 3)
        return false;
    NewFrame(frame);
    pic = LookupHash(HashPic(tiles), &rows);
    if (pic < 0)
        return false;
    /* Carry on the animation of the sprite the picture came from (hidden
     * now, seen last frame), else start one for the layer. */
    for (int i = 0; i < NUM_INSTANCES; i++)
    {
        Instance *in = &sInstances[i];

        if (in->used && sPics[in->pic].entry == sPics[pic].entry && in->lastSeen + 2 >= sFrame)
        {
            in->lastSeen = sFrame;
            start = in->start;
            found = true;
            break;
        }
    }
    if (!found)
        start = StartFrameFor((uint16_t)(0x8000 | bg), (uint16_t)pic);
    return FinishSprite(128 + bg, pic, rows, start, palette, &sBgShiny[paletteSlot & 15], out);
}

bool CtrBw_Prepare(unsigned index, const uint16_t *obj, bool mapping1d, const uint8_t *objVram,
                   const uint16_t *objPalettes, uint32_t frame, CtrBwSprite *out)
{
    static uint8_t tiles[PIC_FRAME_SIZE];
    uint16_t a0 = obj[0], a1 = obj[1], a2 = obj[2], tileBase = a2 & 0x3ff;
    uint8_t rows = 8;
    int pic = -1, k;

    if (!sPack || !sNumPics)
        return false;
    /* 64x64 4bpp sprites. */
    if ((a0 >> 14) != 0 || (a1 >> 14) != 3 || (a0 & 0x2000) || tileBase + 64 > 1024)
        return false;
    NewFrame(frame);
    for (k = 0; k < sNumSeen; k++)
        if (sSeen[k].tileBase == tileBase)
        {
            pic = sSeen[k].pic;
            rows = sSeen[k].rows;
            break;
        }
    if (k == sNumSeen)
    {
        if (mapping1d)
            memcpy(tiles, objVram + tileBase * 32, PIC_FRAME_SIZE);
        else
            for (int row = 0; row < 8; row++)
                memcpy(tiles + row * 256, objVram + ((tileBase + row * 32) & 0x3ff) * 32, 256);
        pic = LookupHash(HashPic(tiles), &rows);
        if (sNumSeen < 16)
        {
            sSeen[sNumSeen].tileBase = tileBase;
            sSeen[sNumSeen].pic = (int16_t)pic;
            sSeen[sNumSeen].rows = rows;
            sNumSeen++;
        }
    }
    if (pic < 0)
        return false;
    return FinishSprite(index, pic, rows, StartFrameFor(tileBase, (uint16_t)pic), objPalettes + (a2 >> 12) * 16,
                        &sShiny[a2 >> 12], out);
}
