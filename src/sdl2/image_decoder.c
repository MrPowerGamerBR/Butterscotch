#include "image_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

#include "stb_image.h"

#define QOI_HEADER_SIZE 12
#define COMPRESSED_QOI_HEADER_SIZE_OLD 8
#define COMPRESSED_QOI_HEADER_SIZE_NEW 12

// Sign-extend the low "bits" bits of "val" to an 8-bit two's-complement value.
static inline uint8_t signExtend(uint32_t val, int bits) {
    uint32_t mask = 1U << (bits - 1);
    return (uint8_t)((val ^ mask) - mask);
}

// Decodes GameMaker's custom QOI format (ported from UndertaleModTool's QoiConverter).
static uint8_t* decodeQoi(const uint8_t* data, size_t dataSize, int* outW, int* outH) {
    if (QOI_HEADER_SIZE > dataSize) return nullptr;
    if (data[0] != 'f' || data[1] != 'i' || data[2] != 'o' || data[3] != 'q') return nullptr;

    int width = data[4] | (data[5] << 8);
    int height = data[6] | (data[7] << 8);
    uint32_t length = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    if (QOI_HEADER_SIZE + (size_t) length > dataSize) return nullptr;
    if (0 >= width || 0 >= height) return nullptr;

    const uint8_t* pixelData = data + QOI_HEADER_SIZE;
    size_t pixelDataSize = length;
    size_t rawSize = (size_t)width * (size_t)height * 4;
    uint8_t* raw = (uint8_t*) malloc(rawSize);
    if (!raw) return nullptr;

    uint8_t index[64 * 4];
    memset(index, 0, sizeof(index));

    size_t pos = 0;
    int run = 0;
    uint8_t r = 0, g = 0, b = 0, a = 255;

    for (size_t rawDataPos = 0; rawSize > rawDataPos; rawDataPos += 4) {
        if (run > 0) {
            run--;
        } else if (pixelDataSize > pos) {
            uint8_t b1 = pixelData[pos++];

            if ((b1 & 0xC0) == 0x00) {
                // QOI_INDEX
                int indexPos = (b1 & 0x3F) << 2;
                r = index[indexPos];
                g = index[indexPos + 1];
                b = index[indexPos + 2];
                a = index[indexPos + 3];
            } else if ((b1 & 0xE0) == 0x40) {
                // QOI_RUN_8
                run = b1 & 0x1F;
            } else if ((b1 & 0xE0) == 0x60) {
                // QOI_RUN_16
                if (pixelDataSize <= pos) { free(raw); return nullptr; }
                uint8_t b2 = pixelData[pos++];
                run = (((b1 & 0x1F) << 8) | b2) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                // QOI_DIFF_8 (2-2-2 signed deltas on r,g,b)
                r += signExtend((b1 >> 4) & 3, 2);
                g += signExtend((b1 >> 2) & 3, 2);
                b += signExtend(b1 & 3, 2);
            } else if ((b1 & 0xE0) == 0xC0) {
                // QOI_DIFF_16 (5-4-4 signed deltas on r,g,b)
                if (pixelDataSize <= pos) { free(raw); return nullptr; }
                uint8_t b2 = pixelData[pos++];
                uint32_t merged = ((uint32_t)b1 << 8) | b2;
                r += signExtend((merged >> 8) & 0x1F, 5);
                g += signExtend((merged >> 4) & 0x0F, 4);
                b += signExtend(merged & 0x0F, 4);
            } else if ((b1 & 0xF0) == 0xE0) {
                // QOI_DIFF_24 (5-5-5-5 signed deltas on r,g,b,a)
                if (pixelDataSize <= pos + 1) { free(raw); return nullptr; }
                uint8_t b2 = pixelData[pos++];
                uint8_t b3 = pixelData[pos++];
                uint32_t merged = ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
                r += signExtend((merged >> 15) & 0x1F, 5);
                g += signExtend((merged >> 10) & 0x1F, 5);
                b += signExtend((merged >> 5) & 0x1F, 5);
                a += signExtend(merged & 0x1F, 5);
            } else if ((b1 & 0xF0) == 0xF0) {
                // QOI_COLOR (per-channel raw bytes, only those with set bit flag)
                if (b1 & 8) { if (pixelDataSize <= pos) { free(raw); return nullptr; } r = pixelData[pos++]; }
                if (b1 & 4) { if (pixelDataSize <= pos) { free(raw); return nullptr; } g = pixelData[pos++]; }
                if (b1 & 2) { if (pixelDataSize <= pos) { free(raw); return nullptr; } b = pixelData[pos++]; }
                if (b1 & 1) { if (pixelDataSize <= pos) { free(raw); return nullptr; } a = pixelData[pos++]; }
            }

            int indexPos2 = ((r ^ g ^ b ^ a) & 63) << 2;
            index[indexPos2] = r;
            index[indexPos2 + 1] = g;
            index[indexPos2 + 2] = b;
            index[indexPos2 + 3] = a;
        }

        raw[rawDataPos] = r;
        raw[rawDataPos + 1] = g;
        raw[rawDataPos + 2] = b;
        raw[rawDataPos + 3] = a;
    }

    *outW = width;
    *outH = height;
    return raw;
}

// Decodes BZip2-compressed custom QOI. The blob layout is:
//   bytes 0..3 = "2zoq" magic
//   bytes 4..5 = width (LE uint16)
//   bytes 6..7 = height (LE uint16)
//   bytes 8..11 = uncompressed BZ2 length (LE uint32) -- ONLY when gm2022_5 is true
//   bytes 8.. (or 12.. if gm2022_5) = raw BZip2 stream, which decompresses into a full "fioq" QOI file.
static uint8_t* decodeBz2Qoi(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    size_t headerSize = gm2022_5 ? COMPRESSED_QOI_HEADER_SIZE_NEW : COMPRESSED_QOI_HEADER_SIZE_OLD;
    if (headerSize > blobSize) return nullptr;

    int width = blob[4] | (blob[5] << 8);
    int height = blob[6] | (blob[7] << 8);
    if (0 >= width || 0 >= height) return nullptr;

    // Upper bound on decompressed QOI: header size + width*height*5 pixel data.
    size_t uncompressedCapacity = QOI_HEADER_SIZE + (size_t) width * (size_t) height * 5;
    uint8_t* uncompressed = (uint8_t*) malloc(uncompressedCapacity);
    if (!uncompressed) return nullptr;

    unsigned int destLen = (unsigned int) uncompressedCapacity;
    int rc = BZ2_bzBuffToBuffDecompress((char*) uncompressed, &destLen, (char*)(blob + headerSize), (unsigned int)(blobSize - headerSize), 0, 0);
    if (rc != BZ_OK) {
        fprintf(stderr, "ImageDecoder: BZ2 decompress failed (rc=%d)\n", rc);
        free(uncompressed);
        return nullptr;
    }

    uint8_t* result = decodeQoi(uncompressed, destLen, outW, outH);
    free(uncompressed);
    return result;
}

uint8_t* ImageDecoder_decodeToRgba(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH) {
    if (4 > blobSize || !blob) return nullptr;

    // GameMaker custom QOI ("fioq")
    if (blob[0] == 'f' && blob[1] == 'i' && blob[2] == 'o' && blob[3] == 'q') {
        return decodeQoi(blob, blobSize, outW, outH);
    }

    // BZip2 + GameMaker custom QOI ("2zoq")
    if (blob[0] == '2' && blob[1] == 'z' && blob[2] == 'o' && blob[3] == 'q') {
        return decodeBz2Qoi(blob, blobSize, gm2022_5, outW, outH);
    }

    // PNG (or anything else stbi recognizes).
    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(blob, (int) blobSize, &w, &h, &channels, 4);
    if (!pixels) return nullptr;
    *outW = w;
    *outH = h;
    return pixels;
}
