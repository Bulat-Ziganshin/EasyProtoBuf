// Code shared by encoder.hpp and decoder.hpp
#pragma once

#include <string>
#include <cstring>
#include <cstdint>
#include <stdexcept>


namespace easypb
{

enum
{
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


// ****************************************************************************
// Deal with CPU endianness
// ****************************************************************************

// memcpy, which also reverses byte order on big-endian cpus
inline void memcpy_LITTLE_ENDIAN(void* dest, const void* src, int size)
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
    memcpy_LITTLE_ENDIAN(&value, ptr, sizeof(FixedType));
    return value;
}

// Convert the `value` from the native to little-endian byte order
template <typename FixedType>
inline void write_to_little_endian(void* ptr, FixedType value)
{
    memcpy_LITTLE_ENDIAN(ptr, &value, sizeof(FixedType));
}


// ****************************************************************************
// Deal with absence of std::string_view prior to C++17
// ****************************************************************************

// Choose the type to use as easypb::string_view
#ifdef EASYPB_STRING_VIEW

// User-supplied type, e.g. std::string
using string_view = EASYPB_STRING_VIEW;

#elif defined(__cpp_lib_string_view)

// C++17-supplied implementation, if available
using string_view = std::string_view;

#else

// Minimal reimplementation of std::string_view,
// just enough for usage in easypb::Encoder and easypb::Decoder
struct string_view
{
    char* _data;
    size_t _size;

    string_view(const char* data, size_t size)  {_data = (char*)data;  _size = size;}
    string_view(std::string s)  {_data = (char*)(s.data());  _size = s.size();}
    operator std::string()  {return {_data, _size};}
    char*  data()  {return _data;}
    size_t size()  {return _size;}
    char*  begin() {return _data;}
    char*  end()   {return _data + _size;}
};

#endif

}  // namespace easypb
