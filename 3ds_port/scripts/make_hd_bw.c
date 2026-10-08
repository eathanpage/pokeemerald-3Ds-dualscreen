/*
 * make_hd_bw IN OUT: bw_sprites.pack with every frame upscaled 3x, for the 3DS
 * port's 1.5x battle (3ds_bw.c draws it averaged 2x2, at 1.5x).
 *
 * Each frame is upscaled with Scale3x (AdvMAME3x), which only copies pixels:
 * the frames stay colour indices, so the game's colouring of them (shiny,
 * fades, flashes) works as on the originals. The pack keeps its format with
 * version 2: entries' canvas, crop and size are 3x, and frames are stored as
 * before - each XORed with the one before it, then GBA LZ77.
 *
 * Built and run on the PC (cc -O2 -o make_hd_bw make_hd_bw.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint16_t dex;
    uint8_t form, side;
    uint16_t canvasWidth, canvasHeight;
    uint16_t cropX, cropY, width, height;
    uint16_t numFrames, seqLength;
    uint32_t palettesOffset, seqOffset, framesOffset;
} PackEntry;

static uint8_t *ReadFile(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(*size);
    if (!data || fread(data, 1, *size, f) != (size_t)*size) { fclose(f); free(data); return NULL; }
    fclose(f);
    return data;
}

static int Lz77Decode(const uint8_t *src, const uint8_t *end, uint8_t *dest, uint32_t size)
{
    uint32_t total, out = 0;

    if (end - src < 4 || src[0] != 0x10) return 0;
    total = src[1] | (src[2] << 8) | (src[3] << 16);
    if (total != size) return 0;
    src += 4;
    while (out < total && src < end)
    {
        uint8_t flags = *src++;
        for (int bit = 0; bit < 8 && out < total; bit++, flags <<= 1)
        {
            if (!(flags & 0x80))
            {
                if (src >= end) return 0;
                dest[out++] = *src++;
            }
            else
            {
                uint32_t length, disp;
                if (end - src < 2) return 0;
                length = (src[0] >> 4) + 3;
                disp = (((src[0] & 15) << 8) | src[1]) + 1;
                src += 2;
                if (disp > out) return 0;
                while (length-- && out < total) { dest[out] = dest[out - disp]; out++; }
            }
        }
    }
    return out == total;
}

/* GBA LZ77 (type 0x10), greedy longest match with hash chains. Returns the size. */
static uint32_t Lz77Encode(const uint8_t *src, uint32_t size, uint8_t *dest)
{
    enum { WINDOW = 4096, MAXLEN = 18, HASH = 1 << 16 };
    static int32_t head[HASH];
    static int32_t *prev;
    static uint32_t prevSize;
    uint32_t out = 4, pos = 0;

    if (prevSize < size)
    {
        free(prev);
        prev = malloc(size * sizeof(*prev));
        prevSize = size;
    }
    for (int i = 0; i < HASH; i++) head[i] = -1;
    dest[0] = 0x10;
    dest[1] = size & 255;
    dest[2] = (size >> 8) & 255;
    dest[3] = (size >> 16) & 255;
    while (pos < size)
    {
        uint32_t flagPos = out++;
        uint8_t flags = 0;

        for (int bit = 0; bit < 8 && pos < size; bit++)
        {
            uint32_t bestLen = 0, bestDisp = 0;

            if (pos + 2 < size)
            {
                uint32_t h = (src[pos] * 2654435761u ^ src[pos + 1] * 40503u ^ src[pos + 2]) & (HASH - 1);
                int chain = 0;

                for (int32_t cand = head[h]; cand >= 0 && pos - (uint32_t)cand <= WINDOW && chain < 64;
                     cand = prev[cand], chain++)
                {
                    uint32_t len = 0;

                    /* The GBA's decoder needs a displacement of at least 1;
                     * VRAM-safe 2 is not needed here (read from RAM). */
                    while (len < MAXLEN && pos + len < size && src[cand + len] == src[pos + len]) len++;
                    if (len > bestLen) { bestLen = len; bestDisp = pos - (uint32_t)cand; }
                    if (len == MAXLEN) break;
                }
            }
            if (bestLen >= 3)
            {
                flags |= 0x80 >> bit;
                dest[out++] = (uint8_t)(((bestLen - 3) << 4) | ((bestDisp - 1) >> 8));
                dest[out++] = (uint8_t)((bestDisp - 1) & 255);
            }
            else
            {
                bestLen = 1;
                dest[out++] = src[pos];
            }
            for (uint32_t k = 0; k < bestLen; k++, pos++)
                if (pos + 2 < size)
                {
                    uint32_t h = (src[pos] * 2654435761u ^ src[pos + 1] * 40503u ^ src[pos + 2]) & (HASH - 1);
                    prev[pos] = head[h];
                    head[h] = (int32_t)pos;
                }
        }
        dest[flagPos] = flags;
    }
    while (out & 3) dest[out++] = 0;
    return out;
}

static uint8_t Pixel(const uint8_t *frame, int w, int h, int x, int y)
{
    uint8_t b;

    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    b = frame[y * ((w + 1) / 2) + (x >> 1)];
    return (x & 1) ? b >> 4 : b & 15;
}

static void Put(uint8_t *frame, int w, int x, int y, uint8_t v)
{
    uint8_t *b = &frame[y * ((w + 1) / 2) + (x >> 1)];

    *b = (x & 1) ? (uint8_t)((*b & 15) | (v << 4)) : (uint8_t)((*b & 0xF0) | v);
}

static void Scale3x(const uint8_t *in, int w, int h, uint8_t *out)
{
    int ow = w * 3;

    memset(out, 0, (size_t)((ow + 1) / 2) * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t A = Pixel(in, w, h, x - 1, y - 1), B = Pixel(in, w, h, x, y - 1), C = Pixel(in, w, h, x + 1, y - 1);
            uint8_t D = Pixel(in, w, h, x - 1, y), E = Pixel(in, w, h, x, y), F = Pixel(in, w, h, x + 1, y);
            uint8_t G = Pixel(in, w, h, x - 1, y + 1), H = Pixel(in, w, h, x, y + 1), I = Pixel(in, w, h, x + 1, y + 1);
            uint8_t o[9] = {E, E, E, E, E, E, E, E, E};

            if (B != H && D != F)
            {
                o[0] = D == B ? D : E;
                o[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o[2] = B == F ? F : E;
                o[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o[6] = D == H ? D : E;
                o[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o[8] = H == F ? F : E;
            }
            for (int k = 0; k < 9; k++)
                Put(out, ow, x * 3 + k % 3, y * 3 + k / 3, o[k]);
        }
}

int main(int argc, char **argv)
{
    long size;
    uint8_t *pack;
    uint32_t count;
    PackEntry *in, *outEntries;
    FILE *out;
    uint32_t written;

    if (argc != 3) { fprintf(stderr, "make_hd_bw IN OUT\n"); return 1; }
    pack = ReadFile(argv[1], &size);
    if (!pack || size < 16 || memcmp(pack, "BWSP", 4) || ((uint32_t *)pack)[1] != 1)
    {
        fprintf(stderr, "not a version 1 BW pack\n");
        return 1;
    }
    count = ((uint32_t *)pack)[2];
    in = (PackEntry *)(pack + 16);
    outEntries = calloc(count, sizeof(PackEntry));
    out = fopen(argv[2], "wb");
    if (!out) return 1;
    /* The header and index are written last, once the offsets are known. */
    written = 16 + count * sizeof(PackEntry);
    fseek(out, written, SEEK_SET);
    for (uint32_t i = 0; i < count; i++)
    {
        const PackEntry *e = &in[i];
        PackEntry *o = &outEntries[i];
        uint32_t blobEnd = i + 1 < count ? in[i + 1].palettesOffset : (uint32_t)size;
        uint32_t frameBytes = (uint32_t)((e->width + 1) / 2) * e->height;
        int ow = e->width * 3, oh = e->height * 3;
        uint32_t outBytes = (uint32_t)((ow + 1) / 2) * oh;
        uint8_t *frames = calloc(e->numFrames ? e->numFrames : 1, frameBytes);
        uint8_t *big = calloc(e->numFrames ? e->numFrames : 1, outBytes);
        uint8_t *packed = malloc(outBytes + outBytes / 8 + 64);
        uint32_t *offsets = calloc(e->numFrames ? e->numFrames : 1, 4);
        const uint32_t *srcOffsets = (const uint32_t *)(pack + e->framesOffset);

        *o = *e;
        o->canvasWidth = e->canvasWidth * 3;
        o->canvasHeight = e->canvasHeight * 3;
        o->cropX = e->cropX * 3;
        o->cropY = e->cropY * 3;
        o->width = (uint16_t)ow;
        o->height = (uint16_t)oh;
        for (int f = 0; f < e->numFrames; f++)
        {
            uint8_t *frame = frames + (size_t)f * frameBytes;

            if (srcOffsets[f] >= blobEnd || !Lz77Decode(pack + srcOffsets[f], pack + blobEnd, frame, frameBytes))
                memset(frame, 0, frameBytes);
            if (f > 0)
                for (uint32_t b = 0; b < frameBytes; b++) frame[b] ^= (frame - frameBytes)[b];
            Scale3x(frame, e->width, e->height, big + (size_t)f * outBytes);
        }
        /* Palettes and sequence as they are. */
        o->palettesOffset = written;
        fwrite(pack + e->palettesOffset, 1, 64, out);
        written += 64;
        o->seqOffset = written;
        fwrite(pack + e->seqOffset, 1, (size_t)e->seqLength * 4, out);
        written += (uint32_t)e->seqLength * 4;
        o->framesOffset = written;
        fwrite(offsets, 4, e->numFrames, out); /* placeholders */
        written += (uint32_t)e->numFrames * 4;
        for (int f = e->numFrames - 1; f > 0; f--)
            for (uint32_t b = 0; b < outBytes; b++)
                big[(size_t)f * outBytes + b] ^= big[(size_t)(f - 1) * outBytes + b];
        for (int f = 0; f < e->numFrames; f++)
        {
            uint32_t n = Lz77Encode(big + (size_t)f * outBytes, outBytes, packed);

            offsets[f] = written;
            fwrite(packed, 1, n, out);
            written += n;
        }
        fseek(out, o->framesOffset, SEEK_SET);
        fwrite(offsets, 4, e->numFrames, out);
        fseek(out, written, SEEK_SET);
        free(frames);
        free(big);
        free(packed);
        free(offsets);
        if (i % 100 == 0) fprintf(stderr, "%u/%u, %u MB\n", i, count, written >> 20);
    }
    {
        uint32_t header[4] = {0, 2, count, 0};

        memcpy(header, "BWSP", 4);
        fseek(out, 0, SEEK_SET);
        fwrite(header, 4, 4, out);
        fwrite(outEntries, sizeof(PackEntry), count, out);
    }
    fclose(out);
    printf("%u entries, %u bytes\n", count, written);
    return 0;
}
