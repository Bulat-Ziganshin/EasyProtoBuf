// Code shared by encoder.hpp and decoder.hpp
#pragma once

namespace easypb
{

enum {
    MAX_VARINT_SIZE = (64+6)/7,  // number of 7-bit chunks in 64-bit int
    MAX_LENGTH_CODE_SIZE = (32+6)/7,  // number of 7-bit chunks in 32-bit int encoding message length
};

enum WireType
{
    WIRETYPE_UNDEFINED = -1,
    WIRETYPE_VARINT = 0,
    WIRETYPE_FIXED64 = 1,
    WIRETYPE_LENGTH_DELIMITED = 2,
    WIRETYPE_START_GROUP = 3,
    WIRETYPE_END_GROUP = 4,
    WIRETYPE_FIXED32 = 5,
};

// memcpy, which also reverses byte order on big-endian cpus
inline void memcpy_BYTE_ORDER(void* dest, const void* src, int size)
{
    // Equivalent to "#if __BYTE_ORDER == __BIG_ENDIAN", but more portable.
    // If cpu has PDP byte order, or floats and ints have different order, you are out of luck.
    if((*(uint16_t *)"\0\xff" < 0x100)) {
        auto to = (char*) dest;
        auto from = (const char*) src;
        if (size == 4) {
            to[0] = from[3];
            to[1] = from[2];
            to[2] = from[1];
            to[3] = from[0];
        } else {
            to[0] = from[7];
            to[1] = from[6];
            to[2] = from[5];
            to[3] = from[4];
            to[4] = from[3];
            to[5] = from[2];
            to[6] = from[1];
            to[7] = from[0];
        }
    } else {
        memcpy(dest, src, size);
    }
}

// Convert the `value` from little-endian to the native byte order
template <typename FixedType>
inline FixedType read_from_little_endian(const void* ptr)
{
    FixedType value;
    memcpy_BYTE_ORDER(&value, ptr, sizeof(FixedType));
    return value;
}

// Convert the `value` from the native to little-endian byte order
template <typename FixedType>
inline void write_to_little_endian(void* ptr, FixedType value)
{
    memcpy_BYTE_ORDER(ptr, &value, sizeof(FixedType));
}

}  // namespace easypb
