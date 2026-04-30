#pragma once

#include "common.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Binary reads/writes from a raw byte buffer.
// When IS_BIG_ENDIAN is defined, reads are byte-swapped to interpret serialized little-endian data.

#if defined(__clang__) || defined(__GNUC__)
static inline uint16_t BinaryUtils_bswap16(uint16_t value) {
    return __builtin_bswap16(value);
}

static inline uint32_t BinaryUtils_bswap32(uint32_t value) {
    return __builtin_bswap32(value);
}

static inline uint64_t BinaryUtils_bswap64(uint64_t value) {
    return __builtin_bswap64(value);
}
#elif defined(_MSC_VER)
static inline uint16_t BinaryUtils_bswap16(uint16_t value) {
    return _byteswap_ushort(value);
}

static inline uint32_t BinaryUtils_bswap32(uint32_t value) {
    return _byteswap_ulong(value);
}

static inline uint64_t BinaryUtils_bswap64(uint64_t value) {
    return _byteswap_uint64(value);
}
#else
static inline uint16_t BinaryUtils_bswap16(uint16_t value) {
    return (uint16_t) ((value >> 8) | (value << 8));
}

static inline uint32_t BinaryUtils_bswap32(uint32_t value) {
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8)  |
           ((value & 0x00FF0000u) >> 8)  |
           ((value & 0xFF000000u) >> 24);
}

static inline uint64_t BinaryUtils_bswap64(uint64_t value) {
    return ((value & 0x00000000000000FFull) << 56) |
           ((value & 0x000000000000FF00ull) << 40) |
           ((value & 0x0000000000FF0000ull) << 24) |
           ((value & 0x00000000FF000000ull) << 8)  |
           ((value & 0x000000FF00000000ull) >> 8)  |
           ((value & 0x0000FF0000000000ull) >> 24) |
           ((value & 0x00FF000000000000ull) >> 40) |
           ((value & 0xFF00000000000000ull) >> 56);
}
#endif

static inline uint16_t BinaryUtils_toLittle16(uint16_t value) {
#if defined(IS_BIG_ENDIAN)
    return BinaryUtils_bswap16(value);
#else
    return value;
#endif
}

static inline uint32_t BinaryUtils_toLittle32(uint32_t value) {
#if defined(IS_BIG_ENDIAN)
    return BinaryUtils_bswap32(value);
#else
    return value;
#endif
}

static inline uint64_t BinaryUtils_toLittle64(uint64_t value) {
#if defined(IS_BIG_ENDIAN)
    return BinaryUtils_bswap64(value);
#else
    return value;
#endif
}

static inline uint8_t BinaryUtils_readUint8(const uint8_t* data) {
    return data[0];
}

static inline uint16_t BinaryUtils_readUint16(const uint8_t* data) {
    uint16_t val;
    memcpy(&val, data, 2);
    return BinaryUtils_toLittle16(val);
}

static inline int16_t BinaryUtils_readInt16(const uint8_t* data) {
    return (int16_t) BinaryUtils_readUint16(data);
}

static inline uint32_t BinaryUtils_readUint32(const uint8_t* data) {
    uint32_t val;
    memcpy(&val, data, 4);
    return BinaryUtils_toLittle32(val);
}

static inline int32_t BinaryUtils_readInt32(const uint8_t* data) {
    return (int32_t) BinaryUtils_readUint32(data);
}

static inline uint64_t BinaryUtils_readUint64(const uint8_t* data) {
    uint64_t val;
    memcpy(&val, data, 8);
    return BinaryUtils_toLittle64(val);
}

static inline int64_t BinaryUtils_readInt64(const uint8_t* data) {
    return (int64_t) BinaryUtils_readUint64(data);
}

static inline float BinaryUtils_readFloat32(const uint8_t* data) {
    uint32_t bits = BinaryUtils_readUint32(data);
    float val;
    memcpy(&val, &bits, 4);
    return val;
}

static inline double BinaryUtils_readFloat64(const uint8_t* data) {
    uint64_t bits = BinaryUtils_readUint64(data);
    double val;
    memcpy(&val, &bits, 8);
    return val;
}

static inline void BinaryUtils_writeUint32(uint8_t* data, uint32_t val) {
    val = BinaryUtils_toLittle32(val);
    memcpy(data, &val, 4);
}

static inline void BinaryUtils_writeUint16(uint8_t* data, uint16_t val) {
    val = BinaryUtils_toLittle16(val);
    memcpy(data, &val, 2);
}

static inline void BinaryUtils_writeFloat32(uint8_t* data, float val) {
    uint32_t bits;
    memcpy(&bits, &val, 4);
    bits = BinaryUtils_toLittle32(bits);
    memcpy(data, &bits, 4);
}

static inline void BinaryUtils_writeFloat64(uint8_t* data, double val) {
    uint64_t bits;
    memcpy(&bits, &val, 8);
    bits = BinaryUtils_toLittle64(bits);
    memcpy(data, &bits, 8);
}

static inline void BinaryUtils_writeUint64(uint8_t* data, uint64_t val) {
    val = BinaryUtils_toLittle64(val);
    memcpy(data, &val, 8);
}

static inline void BinaryUtils_writeInt64(uint8_t* data, int64_t val) {
    BinaryUtils_writeUint64(data, (uint64_t) val);
}

// ===[ Aligned reads ]===
// These trust the caller to supply a pointer with matching natural alignment.
// Used on the VM dispatch hot path (bytecode instruction / operand fetch) where the bytecode buffer is guaranteed 4-byte aligned.

static inline uint32_t BinaryUtils_readUint32Aligned(const uint8_t* data) {
    uint32_t val;
    memcpy(&val, __builtin_assume_aligned(data, 4), 4);
    return BinaryUtils_toLittle32(val);
}

static inline int32_t BinaryUtils_readInt32Aligned(const uint8_t* data) {
    return (int32_t) BinaryUtils_readUint32Aligned(data);
}

static inline int64_t BinaryUtils_readInt64Aligned(const uint8_t* data) {
    // Note: GML bytecode places 8-byte extra-data at instruction + 4, so it is only 4-aligned.
    uint64_t val;
    memcpy(&val, __builtin_assume_aligned(data, 4), 8);
    return (int64_t) BinaryUtils_toLittle64(val);
}

static inline float BinaryUtils_readFloat32Aligned(const uint8_t* data) {
    uint32_t bits;
    memcpy(&bits, __builtin_assume_aligned(data, 4), 4);
    bits = BinaryUtils_toLittle32(bits);
    float val;
    memcpy(&val, &bits, 4);
    return val;
}

static inline double BinaryUtils_readFloat64Aligned(const uint8_t* data) {
    // Note: GML bytecode places 8-byte extra-data at instruction + 4, so it is only 4-aligned.
    uint64_t bits;
    memcpy(&bits, __builtin_assume_aligned(data, 4), 8);
    bits = BinaryUtils_toLittle64(bits);
    double val;
    memcpy(&val, &bits, 8);
    return val;
}
