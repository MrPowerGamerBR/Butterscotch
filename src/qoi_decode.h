#pragma once

// GameMaker custom QOI decoder (supports both 'fioq' and BZ2-compressed '2zoq')
// Based on the GM-QOI format used in GMS 2022.5+
// Reference: UndertaleModTool's QoiConverter.cs (ported from DogScepter)

#include "common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

// Decodes raw GM-QOI pixel data (after the 12-byte 'fioq' header) into RGBA pixels.
// Returns a malloc'd RGBA pixel buffer (caller must free), or nullptr on failure.
static uint8_t* qoiDecodePixels(const uint8_t* pixelData, uint32_t pixelDataLen, int width, int height) {
    int totalPixels = width * height;
    uint8_t* output = malloc((size_t) totalPixels * 4);
    if (output == nullptr) return nullptr;

    uint8_t index[64 * 4];
    memset(index, 0, sizeof(index));

    int pos = 0;
    int run = 0;
    uint8_t r = 0, g = 0, b = 0, a = 255;

    for (int outPos = 0; outPos < totalPixels * 4; outPos += 4) {
        if (run > 0) {
            run--;
        } else if ((uint32_t) pos < pixelDataLen) {
            int b1 = pixelData[pos++];

            if ((b1 & 0xC0) == 0x00) {
                // QOI_INDEX: 6-bit color index lookup
                int indexPos = (b1 ^ 0x00) << 2;
                r = index[indexPos];
                g = index[indexPos + 1];
                b = index[indexPos + 2];
                a = index[indexPos + 3];
            } else if ((b1 & 0xE0) == 0x40) {
                // QOI_RUN_8: 5-bit run length (1-32)
                run = b1 & 0x1F;
            } else if ((b1 & 0xE0) == 0x60) {
                // QOI_RUN_16: 13-bit run length (33-8224)
                int b2 = pixelData[pos++];
                run = (((b1 & 0x1F) << 8) | b2) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                // QOI_DIFF_8: 2-bit signed deltas for RGB
                r += (uint8_t) (((b1 & 48) << 26 >> 30) & 0xFF);
                g += (uint8_t) (((b1 & 12) << 28 >> 22 >> 8) & 0xFF);
                b += (uint8_t) (((b1 & 3) << 30 >> 14 >> 16) & 0xFF);
            } else if ((b1 & 0xE0) == 0xC0) {
                // QOI_DIFF_16: 5-bit red, 4-bit green/blue signed deltas
                int b2 = pixelData[pos++];
                int merged = (b1 << 8) | b2;
                r += (uint8_t) (((merged & 7936) << 19 >> 27) & 0xFF);
                g += (uint8_t) (((merged & 240) << 24 >> 20 >> 8) & 0xFF);
                b += (uint8_t) (((merged & 15) << 28 >> 12 >> 16) & 0xFF);
            } else if ((b1 & 0xF0) == 0xE0) {
                // QOI_DIFF_24: 4/5/5/5-bit signed deltas for RGBA
                int b2 = pixelData[pos++];
                int b3 = pixelData[pos++];
                int merged = (b1 << 16) | (b2 << 8) | b3;
                r += (uint8_t) (((merged & 1015808) << 12 >> 27) & 0xFF);
                g += (uint8_t) (((merged & 31744) << 17 >> 19 >> 8) & 0xFF);
                b += (uint8_t) (((merged & 992) << 22 >> 11 >> 16) & 0xFF);
                a += (uint8_t) (((merged & 31) << 27 >> 3 >> 24) & 0xFF);
            } else if ((b1 & 0xF0) == 0xF0) {
                // QOI_COLOR: explicit channel values
                if (b1 & 8) r = pixelData[pos++];
                if (b1 & 4) g = pixelData[pos++];
                if (b1 & 2) b = pixelData[pos++];
                if (b1 & 1) a = pixelData[pos++];
            }

            // Update index table
            int indexPos2 = ((r ^ g ^ b ^ a) & 63) << 2;
            index[indexPos2] = r;
            index[indexPos2 + 1] = g;
            index[indexPos2 + 2] = b;
            index[indexPos2 + 3] = a;
        }

        // Output in RGBA order (the reference outputs BGRA, but we want RGBA for OpenGL)
        output[outPos] = r;
        output[outPos + 1] = g;
        output[outPos + 2] = b;
        output[outPos + 3] = a;
    }

    return output;
}

// Decodes a GM-QOI image blob ('fioq' format) into RGBA pixels.
static uint8_t* qoiDecode(const uint8_t* data, uint32_t dataSize, int* outWidth, int* outHeight) {
    if (dataSize < 12) return nullptr;
    if (data[0] != 'f' || data[1] != 'i' || data[2] != 'o' || data[3] != 'q') return nullptr;

    int width = data[4] | (data[5] << 8);
    int height = data[6] | (data[7] << 8);
    uint32_t compressedLength = (uint32_t) data[8] | ((uint32_t) data[9] << 8) | ((uint32_t) data[10] << 16) | ((uint32_t) data[11] << 24);

    if (width <= 0 || height <= 0) return nullptr;
    if (12 + compressedLength > dataSize) return nullptr;

    *outWidth = width;
    *outHeight = height;
    return qoiDecodePixels(data + 12, compressedLength, width, height);
}

// Decodes a BZ2-compressed GM-QOI image blob ('2zoq' format) into RGBA pixels.
// Format: [magic:4][width:2][height:2][uncompressedLen:4][BZ2 data...]
// The BZ2 data decompresses to a full 'fioq' QOI file.
static uint8_t* qoiBz2Decode(const uint8_t* data, uint32_t dataSize, int* outWidth, int* outHeight) {
    if (dataSize < 12) return nullptr;
    if (data[0] != '2' || data[1] != 'z' || data[2] != 'o' || data[3] != 'q') return nullptr;

    // int width = data[4] | (data[5] << 8);  // also in the inner QOI header
    // int height = data[6] | (data[7] << 8);
    uint32_t uncompressedLen = (uint32_t) data[8] | ((uint32_t) data[9] << 8) | ((uint32_t) data[10] << 16) | ((uint32_t) data[11] << 24);

    // BZ2 data starts at offset 12
    const uint8_t* bz2Data = data + 12;
    uint32_t bz2Len = dataSize - 12;

    // Decompress the BZ2 data - it contains a full QOI file with 'fioq' header
    unsigned int destLen = uncompressedLen;
    uint8_t* decompressed = malloc(destLen);
    if (decompressed == nullptr) return nullptr;

    int bz2Result = BZ2_bzBuffToBuffDecompress((char*) decompressed, &destLen, (char*) bz2Data, bz2Len, 0, 0);
    if (bz2Result != BZ_OK) {
        fprintf(stderr, "QOI: BZ2 decompression failed (error %d)\n", bz2Result);
        free(decompressed);
        return nullptr;
    }

    // The decompressed data is a full QOI file - decode it
    uint8_t* pixels = qoiDecode(decompressed, destLen, outWidth, outHeight);
    free(decompressed);
    return pixels;
}

// Decodes a texture blob that may be PNG, QOI ('fioq'), or BZ2+QOI ('2zoq').
// Returns a malloc'd RGBA pixel buffer, or nullptr on failure.
static uint8_t* textureDecodeBlob(const uint8_t* data, uint32_t dataSize, int* outWidth, int* outHeight) {
    if (data == nullptr || dataSize < 4) return nullptr;

    if (data[0] == '2' && data[1] == 'z' && data[2] == 'o' && data[3] == 'q') {
        return qoiBz2Decode(data, dataSize, outWidth, outHeight);
    }
    if (data[0] == 'f' && data[1] == 'i' && data[2] == 'o' && data[3] == 'q') {
        return qoiDecode(data, dataSize, outWidth, outHeight);
    }
    return nullptr; // not a QOI format, caller should try PNG
}
