// SPDX-License-Identifier: Unlicense
/*
This header file contains the entire EasyProtoBuf library.
It consists of 3 big sections:
- Utility functions shared by Encoder and Decoder
- Encoder class
- Decoder class
*/
#pragma once

#include <string>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <stdexcept>


namespace easypb
{

// ****************************************************************************
// Utility functions shared by Encoder and Decoder
// ****************************************************************************
enum
{
    MAX_VARINT_SIZE = (64+6)/7,  // number of 7-bit chunks in 64-bit int
    MAX_LENGTH_CODE_SIZE = (32+6)/7,  // number of 7-bit chunks in 32-bit int encoding message length
    FIELDNUM_SCALE = 8,  // scales field_num to field_tag
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
// Define the hierarchy of exceptions thrown by the library
// ****************************************************************************

#define EASYPB_DEFINE_EXCEPTION(NEW_TYPE,BASE_TYPE)                           \
    struct NEW_TYPE : BASE_TYPE {                                             \
        NEW_TYPE(const std::string& what_arg)  : BASE_TYPE(what_arg) {}       \
        NEW_TYPE(const char* what_arg)         : BASE_TYPE(what_arg) {}       \
    };                                                                        \

EASYPB_DEFINE_EXCEPTION(exception,              std::runtime_error)
EASYPB_DEFINE_EXCEPTION(unexpected_eof,         exception)
EASYPB_DEFINE_EXCEPTION(varint_too_long,        exception)
EASYPB_DEFINE_EXCEPTION(length_too_long,        exception)
EASYPB_DEFINE_EXCEPTION(invalid_fieldnum,       exception)
EASYPB_DEFINE_EXCEPTION(wiretype_mismatch,      exception)
EASYPB_DEFINE_EXCEPTION(unsupported_wiretype,   exception)
EASYPB_DEFINE_EXCEPTION(missing_required_field, exception)

#undef EASYPB_DEFINE_EXCEPTION


// ****************************************************************************
// Deal with CPU endianness. Convert between the strictly little-endian
// Protobuf wire format and native byte order of the target CPU.
// ****************************************************************************

// memcpy, which also reverses byte order on big-endian cpus
template <typename FixedType>
inline void memcpy_LITTLE_ENDIAN(void* dest, const void* src)
{
    constexpr size_t size = sizeof(FixedType);
    static_assert(size==4 || size==8, "Only size==4 and size==8 are supported");

    // Check whether CPU is big-endian. If cpu has PDP byte order, or floats and ints have different order, you are screwed.
    const uint16_t endianness = 1;
    if (*(uint8_t *)&endianness == 0) {
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
        std::memcpy(dest, src, size);
    }
}

// Convert the `value` from little-endian to the native byte order
template <typename FixedType>
inline FixedType read_from_little_endian(const void* ptr)
{
    FixedType value;
    memcpy_LITTLE_ENDIAN<FixedType>(&value, ptr);
    return value;
}

// Convert the `value` from the native to little-endian byte order
template <typename FixedType>
inline void write_to_little_endian(void* ptr, FixedType value)
{
    memcpy_LITTLE_ENDIAN<FixedType>(ptr, &value);
}


// ****************************************************************************
// Deal with absence of std::string_view prior to C++17
// ****************************************************************************

// Choose the type to use as easypb::string_view: ...
#ifdef EASYPB_STRING_VIEW

// ... either user-supplied type, e.g. std::string
using string_view = EASYPB_STRING_VIEW;

#elif defined(__cpp_lib_string_view)

// ... or C++17-supplied type, if available
using string_view = std::string_view;

#else

// ... or minimal reimplementation of std::string_view,
// just enough for usage in easypb::Encoder and easypb::Decoder
struct string_view
{
    char* _data;
    size_t _size;

    string_view(const char* data, size_t size)  {_data = (char*)data;  _size = size;}
    string_view(const std::string &s)  {_data = (char*)(s.data());  _size = s.size();}
    operator std::string() const  {return {_data, _size};}
    char*  data()  const {return _data;}
    size_t size()  const {return _size;}
    char*  begin() const {return _data;}
    char*  end()   const {return _data + _size;}
};

#endif



// ****************************************************************************
// Class for encoding C++ data into the Protobuf wire format
// ****************************************************************************
struct Encoder
{
    // Invariants:
    //   buf_end == buffer.data() + buffer.size()
    //   buffer.data() <= ptr <= buf_end

    std::string buffer; // buffer storing the serialized data
    char* ptr;          // the current writing point
    char* buf_end;      // end of the allocated space
    char* begin() const {return (char*)(buffer.data());}  // start of the allocated space
    size_t pos()  const {return ptr - begin();}           // the current writing index


    Encoder()
    {
        ptr = buf_end = begin();
    }

    // Return the buffer collected by Encoder, and start from scratch
    std::string result()
    {
        buffer.resize(pos());
        buffer.shrink_to_fit();

        std::string temp_buffer;
        std::swap(buffer, temp_buffer);
        ptr = buf_end = begin();

        return temp_buffer;
    }

    char* advance_ptr(ptrdiff_t bytes)
    {
        if (buf_end - ptr < bytes)
        {
            size_t old_pos = pos();
            buffer.resize(buffer.size()*2 + bytes);
            ptr = begin() + old_pos;
            buf_end = begin() + buffer.size();
        }
        ptr += bytes;
        return ptr - bytes;
    }


    template <typename FixedType>
    void write_fixed_width(FixedType value)
    {
        auto old_ptr = advance_ptr(sizeof(value));
        write_to_little_endian(old_ptr, value);
    }

    void write_varint(uint64_t value)
    {
        ptr = advance_ptr(MAX_VARINT_SIZE);  // reserve enough space

#define EASYPB_STEP(n)                                                  \
{                                                                       \
    auto atom = value >> (n*7);                                         \
    if (atom < 0x80) {                                                  \
        ptr[n] = char(atom);                                            \
        ptr += n + 1;                                                   \
        return;                                                         \
    } else {                                                            \
        ptr[n] = char((atom & 0x7F) | 0x80);                            \
    }                                                                   \
}                                                                       \

        EASYPB_STEP(0)
        EASYPB_STEP(1)
        EASYPB_STEP(2)
        EASYPB_STEP(3)
        EASYPB_STEP(4)
        EASYPB_STEP(5)
        EASYPB_STEP(6)
        EASYPB_STEP(7)
        EASYPB_STEP(8)
        EASYPB_STEP(9)
#undef EASYPB_STEP
        throw std::logic_error("Unreachable: more than 70 bits in uint64_t");
    }

    void write_varint_at(size_t varint_pos, size_t varint_size, uint64_t value)
    {
        auto write_ptr = begin() + varint_pos;
        for (size_t i = 1; i < varint_size; ++i)
        {
            *write_ptr++ = char( (value & 127) | 128 );
            value /= 128;
        }
        *write_ptr++ = char(value);

        if (value > 127) {
            throw length_too_long("Length requires to encode more than " + std::to_string(varint_size) + " bytes");
        }
    }

    void write_zigzag(int64_t value)
    {
        uint64_t x = value;
        write_varint((x << 1) ^ (- int64_t(x >> 63)));
    }

    void write_bytearray(string_view value)
    {
        size_t len = value.size();
        if (len > INT32_MAX) {
            throw length_too_long("Passed byte array is too long with " + std::to_string(len) + " bytes");
        }

        write_varint(len);
        auto start_ptr = advance_ptr(len);
        std::memcpy(start_ptr, value.data(), len);
    }

    void write_field_tag(uint32_t field_num, WireType wire_type)
    {
        write_varint(field_num*FIELDNUM_SCALE + wire_type);
    }

    // Start a length-delimited field with yet unknown size and return its start_pos
    size_t start_length_delimited()
    {
        advance_ptr(MAX_LENGTH_CODE_SIZE);
        return pos();
    }

    // Finish a length-delimited field and fill its length with now-known value
    void commit_length_delimited(size_t start_pos)
    {
        size_t field_len = pos() - start_pos;
        write_varint_at(start_pos - MAX_LENGTH_CODE_SIZE, MAX_LENGTH_CODE_SIZE, field_len);
    }

    template <typename Lambda>
    void write_length_delimited(Lambda code)
    {
        size_t start_pos = start_length_delimited();
        code();
        commit_length_delimited(start_pos);
    }

// Define put_map* method for map<TYPE1,TYPE2>
#define EASYPB_DEFINE_MAP_WRITER(TYPE1, TYPE2)                                \
    template <typename FieldType>                                             \
    void put_map_##TYPE1##_##TYPE2(uint32_t field_num, const FieldType& value)\
    {                                                                         \
        for (const auto& x : value)                                           \
        {                                                                     \
            write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);            \
            write_length_delimited([&]{                                       \
                put_##TYPE1(1, x.first);                                      \
                put_##TYPE2(2, x.second);                                     \
            });                                                               \
        }                                                                     \
    }                                                                         \
/* end of EASYPB_DEFINE_MAP_WRITER macro definition */

// Define put_* methods for TYPE and put_map* methods for any map<TYPE,*>
#define EASYPB_DEFINE_WRITERS(TYPE, C_TYPE, WIRETYPE, WRITER)                 \
                                                                              \
    void put_##TYPE(uint32_t field_num, C_TYPE value)                         \
    {                                                                         \
        write_field_tag(field_num, WIRETYPE);                                 \
        WRITER(value);                                                        \
    }                                                                         \
                                                                              \
    template <typename FieldType>                                             \
    void put_repeated_##TYPE(uint32_t field_num, const FieldType& value)      \
    {                                                                         \
        for(const auto &x: value)  put_##TYPE(field_num, x);                  \
    }                                                                         \
                                                                              \
    template <typename FieldType>                                             \
    void put_packed_##TYPE(uint32_t field_num, const FieldType& value)        \
    {                                                                         \
        static_assert(std::is_scalar<C_TYPE>() && sizeof(FieldType*),         \
            "put_packed_" #TYPE " isn't defined according to ProtoBuf format specifications");  \
                                                                              \
        write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);                \
        write_length_delimited([&]{ for(const auto &x: value)  WRITER(x); }); \
    }                                                                         \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, int32)                                     \
    EASYPB_DEFINE_MAP_WRITER(TYPE, int64)                                     \
    EASYPB_DEFINE_MAP_WRITER(TYPE, uint32)                                    \
    EASYPB_DEFINE_MAP_WRITER(TYPE, uint64)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, sfixed32)                                  \
    EASYPB_DEFINE_MAP_WRITER(TYPE, sfixed64)                                  \
    EASYPB_DEFINE_MAP_WRITER(TYPE, fixed32)                                   \
    EASYPB_DEFINE_MAP_WRITER(TYPE, fixed64)                                   \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, sint32)                                    \
    EASYPB_DEFINE_MAP_WRITER(TYPE, sint64)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, bool)                                      \
    EASYPB_DEFINE_MAP_WRITER(TYPE, enum)                                      \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, float)                                     \
    EASYPB_DEFINE_MAP_WRITER(TYPE, double)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, string)                                    \
    EASYPB_DEFINE_MAP_WRITER(TYPE, bytes)                                     \
                                                                              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, message)                                   \
/* end of EASYPB_DEFINE_WRITERS macro definition*/

    EASYPB_DEFINE_WRITERS(int32, int32_t, WIRETYPE_VARINT, write_varint)
    EASYPB_DEFINE_WRITERS(int64, int64_t, WIRETYPE_VARINT, write_varint)
    EASYPB_DEFINE_WRITERS(uint32, uint32_t, WIRETYPE_VARINT, write_varint)
    EASYPB_DEFINE_WRITERS(uint64, uint64_t, WIRETYPE_VARINT, write_varint)

    EASYPB_DEFINE_WRITERS(sfixed32, int32_t, WIRETYPE_FIXED32, write_fixed_width)
    EASYPB_DEFINE_WRITERS(sfixed64, int64_t, WIRETYPE_FIXED64, write_fixed_width)
    EASYPB_DEFINE_WRITERS(fixed32, uint32_t, WIRETYPE_FIXED32, write_fixed_width)
    EASYPB_DEFINE_WRITERS(fixed64, uint64_t, WIRETYPE_FIXED64, write_fixed_width)

    EASYPB_DEFINE_WRITERS(sint32, int32_t, WIRETYPE_VARINT, write_zigzag)
    EASYPB_DEFINE_WRITERS(sint64, int64_t, WIRETYPE_VARINT, write_zigzag)

    EASYPB_DEFINE_WRITERS(bool, bool, WIRETYPE_VARINT, write_varint)
    EASYPB_DEFINE_WRITERS(enum, int32_t, WIRETYPE_VARINT, write_varint)

    EASYPB_DEFINE_WRITERS(float, float, WIRETYPE_FIXED32, write_fixed_width)
    EASYPB_DEFINE_WRITERS(double, double, WIRETYPE_FIXED64, write_fixed_width)

    EASYPB_DEFINE_WRITERS(string, string_view, WIRETYPE_LENGTH_DELIMITED, write_bytearray)
    EASYPB_DEFINE_WRITERS(bytes, string_view, WIRETYPE_LENGTH_DELIMITED, write_bytearray)

#undef EASYPB_DEFINE_MAP_WRITER
#undef EASYPB_DEFINE_WRITERS

    template <typename FieldType>
    void put_message(uint32_t field_num, const FieldType& value)
    {
        write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
        write_length_delimited([&]{ value.encode(*this); });
    }

    template <typename FieldType>
    void put_repeated_message(uint32_t field_num, const FieldType& value)
    {
        for(const auto &x: value)  put_message(field_num, x);
    }
};


template <typename MessageType>
inline std::string encode(const MessageType& msg)
{
    Encoder pb;
    msg.encode(pb);
    return pb.result();
}



/*****************************************************************************
Class for decoding C++ data from the Protobuf wire format.

The Decoder class contains 3 layers:
1) read_varint() and read_fixed_width(), grabbing basic values from an input buffer
2) parse_*_value(), reading a field with known field's type and wiretype
3) get_*(), providing easy-to-use API for users of this class
*****************************************************************************/

// Forward declaration of the function that's called inside the Decoder class,
// but its implementation uses the Decoder class recursively
template <typename MessageType>
inline MessageType decode(string_view buffer);


struct Decoder
{
    // Invariants:
    //   ptr <= buf_end

    // The bytes between ptr and buf_end contain the not-yet-decoded remainder of the message.
    const char* ptr = nullptr;
    const char* buf_end = nullptr;

    // These properties are filled by get_next_field() and make sense only till the entire field is decoded
    uint32_t field_num = UINT32_MAX;
    WireType wire_type = WIRETYPE_UNDEFINED;


    // The Decoder keeps pointers into the data being decoded, so don't free/move them till the decoding is finished
    explicit Decoder(const char* buffer, size_t size) noexcept
        : ptr{buffer}, buf_end{buffer + size}
    {
    }

    explicit Decoder(string_view view) noexcept
        : Decoder(view.data(), view.size())
    {
    }

    // Prohibit Decoder(std::string_view(char*)), since it creates a Decoder with an incorrect bufsize
    explicit Decoder(const char*) = delete;


    // Skip N bytes of the message
    void advance_ptr(ptrdiff_t bytes)
    {
        if (buf_end - ptr < bytes)  throw unexpected_eof("Unexpected end of buffer");
        ptr += bytes;
    }

    // Did we reach the end of the message?
    bool eof() const
    {
        return(ptr >= buf_end);
    }


    // Read any fixed-width field, with conversion from the little-endian Protobuf wire format
    template <typename FixedType>
    FixedType read_fixed_width()
    {
        auto old_ptr = ptr;
        advance_ptr(sizeof(FixedType));
        return read_from_little_endian<FixedType>(old_ptr);
    }

    // Slow version of reading variable-sized integer
    uint64_t read_varint_slow()
    {
        uint64_t value = 0;
        uint64_t byte;
        int shift = 0;

        do {
            if(eof())        throw unexpected_eof("Unexpected end of buffer in varint");
            if(shift >= 64)  throw varint_too_long("More than 10 bytes in varint");

            byte = *(uint8_t*)ptr;
            value |= ((byte & 127) << shift);
            ptr++;  shift += 7;
        }
        while (byte & 128);

        return value;
    }

    // Fast version of reading variable-sized integer
    uint64_t read_varint()
    {
        if(buf_end - ptr < 10)  return read_varint_slow();

        auto p = (uint8_t*)ptr;
        uint64_t value = 0;

#define EASYPB_STEP(n)                                                        \
{                                                                             \
    value |= (uint64_t(p[n] & 127) << (n*7));                                 \
    if(p[n] < 128)  {ptr += n + 1;  return value;}                            \
}                                                                             \

        EASYPB_STEP(0)
        EASYPB_STEP(1)
        EASYPB_STEP(2)
        EASYPB_STEP(3)
        EASYPB_STEP(4)
        EASYPB_STEP(5)
        EASYPB_STEP(6)
        EASYPB_STEP(7)
        EASYPB_STEP(8)
        EASYPB_STEP(9)
#undef EASYPB_STEP
        throw varint_too_long("More than 10 bytes in varint");
    }

    // Read zigzag-encoded integer value
    int64_t read_zigzag()
    {
        uint64_t value = read_varint();
        return (value >> 1) ^ (- int64_t(value & 1));
    }


    template <typename FloatingPointType>
    FloatingPointType parse_fp_value()
    {
        switch(wire_type) {
            case WIRETYPE_FIXED64:  return FloatingPointType( read_fixed_width<double>() );  // Here we can lose FP precision/range
            case WIRETYPE_FIXED32:  return FloatingPointType( read_fixed_width<float>() );
            default:                throw wiretype_mismatch("Can't parse floating-point value with wiretype "
                                            + std::to_string(wire_type));
        }
    }

    uint64_t parse_integer_value()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_varint();
            case WIRETYPE_FIXED64:  return read_fixed_width<uint64_t>();
            case WIRETYPE_FIXED32:  return read_fixed_width<uint32_t>();
            default:                throw wiretype_mismatch("Can't parse integral value with wiretype "
                                            + std::to_string(wire_type));
        }
    }

    int64_t parse_zigzag_value()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_zigzag();
            case WIRETYPE_FIXED64:  return read_fixed_width<int64_t>();
            case WIRETYPE_FIXED32:  return read_fixed_width<int32_t>();
            default:                throw wiretype_mismatch("Can't parse zigzag integral value with wiretype "
                                            + std::to_string(wire_type));
        }
    }

    string_view parse_bytearray_value()
    {
        if (wire_type != WIRETYPE_LENGTH_DELIMITED) {
            throw wiretype_mismatch("Can't parse bytearray with wiretype " + std::to_string(wire_type));
        }

        uint64_t len = read_varint();
        if (len > INT32_MAX) {
            throw length_too_long("Byte array field is too long with " + std::to_string(len) + " bytes");
        }

        advance_ptr(int32_t(len));

        return {ptr-len, size_t(len)};
    }


    // Read and decode tag of the next field, and prepare to read the field value
    bool get_next_field()
    {
        if(eof())  return false;

        uint64_t tag = read_varint();
        if (tag > UINT32_MAX) {
            throw invalid_fieldnum("Field tag is too large: " + std::to_string(tag));
        }

        field_num = uint32_t(tag / FIELDNUM_SCALE);
        wire_type = WireType(tag % FIELDNUM_SCALE);

        return true;
    }

    // Skip the field value, can be called only after get_next_field() if we choose to ignore the field value
    void skip_field()
    {
        if (wire_type == WIRETYPE_VARINT) {
            read_varint();
        } else if (wire_type == WIRETYPE_FIXED32) {
            advance_ptr(4);
        } else if (wire_type == WIRETYPE_FIXED64) {
            advance_ptr(8);
        } else if (wire_type == WIRETYPE_LENGTH_DELIMITED) {
            uint64_t len = read_varint();
            if (len > INT32_MAX) {
                throw length_too_long("Byte array field is too long with " + std::to_string(len) + " bytes");
            }
            advance_ptr(int32_t(len));
        } else {
            throw unsupported_wiretype("Unsupported wire type " + std::to_string(wire_type));
        }
    }


// Define get_map* method for map<TYPE1,TYPE2>
#define EASYPB_DEFINE_MAP_READER(TYPE1, TYPE2)                                \
    template <typename FieldType>                                             \
    void get_map_##TYPE1##_##TYPE2(FieldType *field)                          \
    {                                                                         \
        Decoder sub_decoder(parse_bytearray_value());                         \
        bool has_key = false, has_value = false;                              \
        typename FieldType::key_type key{};                                   \
        typename FieldType::mapped_type value{};                              \
                                                                              \
        while (sub_decoder.get_next_field())                                  \
        {                                                                     \
            switch (sub_decoder.field_num)                                    \
            {                                                                 \
                case 1: sub_decoder.get_##TYPE1(&key, &has_key); break;       \
                case 2: sub_decoder.get_##TYPE2(&value, &has_value); break;   \
                default: sub_decoder.skip_field();                            \
            }                                                                 \
        }                                                                     \
                                                                              \
        if (has_key && has_value) {                                           \
            (*field)[key] = value;                                            \
        }                                                                     \
    }                                                                         \
/* end of EASYPB_DEFINE_MAP_READER macro definition */

// Define get_* methods for TYPE and get_map* methods for any map<TYPE,*>
#define EASYPB_DEFINE_READERS(TYPE, C_TYPE, PARSER, READER)                   \
                                                                              \
    C_TYPE get_##TYPE()                                                       \
    {                                                                         \
        using FieldType = C_TYPE;                                             \
        return FieldType(PARSER());                                           \
    }                                                                         \
                                                                              \
    template <typename FieldType>                                             \
    void get_##TYPE(FieldType *field, bool *has_field = nullptr)              \
    {                                                                         \
        *field = FieldType(PARSER());                                         \
        if(has_field)  *has_field = true;                                     \
    }                                                                         \
                                                                              \
    template <typename RepeatedFieldType>                                     \
    void get_repeated_##TYPE(RepeatedFieldType *field)                        \
    {                                                                         \
        using FieldType = typename RepeatedFieldType::value_type;             \
                                                                              \
        if (std::is_scalar<C_TYPE>()  &&  (wire_type == WIRETYPE_LENGTH_DELIMITED)) {  \
            /* Parsing packed repeated field */                               \
            Decoder sub_decoder(parse_bytearray_value());                     \
            while (! sub_decoder.eof()) {                                     \
                field->push_back( FieldType(sub_decoder.READER()) );          \
            }                                                                 \
        } else {                                                              \
            field->push_back( FieldType(PARSER()) );                          \
        }                                                                     \
    }                                                                         \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, int32)                                     \
    EASYPB_DEFINE_MAP_READER(TYPE, int64)                                     \
    EASYPB_DEFINE_MAP_READER(TYPE, uint32)                                    \
    EASYPB_DEFINE_MAP_READER(TYPE, uint64)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, sfixed32)                                  \
    EASYPB_DEFINE_MAP_READER(TYPE, sfixed64)                                  \
    EASYPB_DEFINE_MAP_READER(TYPE, fixed32)                                   \
    EASYPB_DEFINE_MAP_READER(TYPE, fixed64)                                   \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, sint32)                                    \
    EASYPB_DEFINE_MAP_READER(TYPE, sint64)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, bool)                                      \
    EASYPB_DEFINE_MAP_READER(TYPE, enum)                                      \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, float)                                     \
    EASYPB_DEFINE_MAP_READER(TYPE, double)                                    \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, string)                                    \
    EASYPB_DEFINE_MAP_READER(TYPE, bytes)                                     \
                                                                              \
    EASYPB_DEFINE_MAP_READER(TYPE, message)                                   \
/* end of EASYPB_DEFINE_READERS macro definition */

    EASYPB_DEFINE_READERS(int32, int32_t, parse_integer_value, read_varint)
    EASYPB_DEFINE_READERS(int64, int64_t, parse_integer_value, read_varint)
    EASYPB_DEFINE_READERS(uint32, uint32_t, parse_integer_value, read_varint)
    EASYPB_DEFINE_READERS(uint64, uint64_t, parse_integer_value, read_varint)

    EASYPB_DEFINE_READERS(sfixed32, int32_t, parse_integer_value, read_fixed_width<int32_t>)
    EASYPB_DEFINE_READERS(sfixed64, int64_t, parse_integer_value, read_fixed_width<int64_t>)
    EASYPB_DEFINE_READERS(fixed32, uint32_t, parse_integer_value, read_fixed_width<uint32_t>)
    EASYPB_DEFINE_READERS(fixed64, uint64_t, parse_integer_value, read_fixed_width<uint64_t>)

    EASYPB_DEFINE_READERS(sint32, int32_t, parse_zigzag_value, read_zigzag)
    EASYPB_DEFINE_READERS(sint64, int64_t, parse_zigzag_value, read_zigzag)

    EASYPB_DEFINE_READERS(bool, bool, parse_integer_value, read_varint)
    EASYPB_DEFINE_READERS(enum, int32_t, parse_integer_value, read_varint)

    EASYPB_DEFINE_READERS(float, float, parse_fp_value<FieldType>, read_fixed_width<float>)
    EASYPB_DEFINE_READERS(double, double, parse_fp_value<FieldType>, read_fixed_width<double>)

    EASYPB_DEFINE_READERS(string, string_view, parse_bytearray_value, parse_bytearray_value)
    EASYPB_DEFINE_READERS(bytes, string_view, parse_bytearray_value, parse_bytearray_value)

#undef EASYPB_DEFINE_MAP_READER
#undef EASYPB_DEFINE_READERS

    template <typename MessageType>
    void get_message(MessageType *field, bool *has_field = nullptr)
    {
        field->decode( Decoder(parse_bytearray_value()) );
        if(has_field)  *has_field = true;
    }

    template <typename RepeatedMessageType>
    void get_repeated_message(RepeatedMessageType *field)
    {
        using T = typename RepeatedMessageType::value_type;
        field->push_back( decode<T>(parse_bytearray_value()) );
    }
};


template <typename MessageType>
inline MessageType decode(string_view buffer)
{
    MessageType msg;
    msg.decode( Decoder(buffer) );
    return msg;
}

}  // namespace easypb
