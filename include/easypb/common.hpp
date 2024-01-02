// Code shared by encoder.hpp and decoder.hpp
#pragma once

// memcpy, which also reverses byte order on big-endian cpus
inline void easypb_memcpy_BYTE_ORDER(void* dest, const void* src, int size)
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
inline FixedType easypb_read_from_little_endian(const void* ptr)
{
    FixedType value;
    easypb_memcpy_BYTE_ORDER(&value, ptr, sizeof(FixedType));
    return value;
}

// Convert the `value` from the native to little-endian byte order
template <typename FixedType>
inline void easypb_write_to_little_endian(void* ptr, FixedType value)
{
    easypb_memcpy_BYTE_ORDER(ptr, &value, sizeof(FixedType));
}
