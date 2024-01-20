// The entire EasyProtoBuf library
#pragma once

#include <string>
#include <cstring>
#include <cstdint>
#include <stdexcept>


namespace easypb
{

// Code shared by Encoder and Decoder
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



struct Encoder
{
    // Invariants:
    //   buf_end == buffer.data() + buffer.size()
    //   buffer.data() <= ptr <= buf_end

    std::string buffer; // buffer storing the serialized data
    char* ptr;          // the current writing point
    char* buf_end;      // end of the allocated space
    char* begin() {return (char*)(buffer.data());}  // start of the allocated space
    size_t pos()  {return ptr - begin();}           // the current writing index


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

    char* advance_ptr(int bytes)
    {
        if(buf_end - ptr < bytes)
        {
            auto old_pos = pos();
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

#define STEP(n)                                                         \
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

        STEP(0)
        STEP(1)
        STEP(2)
        STEP(3)
        STEP(4)
        STEP(5)
        STEP(6)
        STEP(7)
        STEP(8)
        STEP(9)
#undef STEP
        throw std::runtime_error("Unreachable: more than 70 bits in uint64_t");
    }

    void write_varint_at(size_t varint_pos, size_t varint_size, uint64_t value)
    {
        auto ptr = begin() + varint_pos;
        for (int i = 1; i < varint_size; ++i)
        {
            *ptr++ = (value & 127) | 128;
            value /= 128;
        }
        *ptr++ = value;
    }

    void write_zigzag(int64_t value)
    {
        uint64_t x = value;
        write_varint((x << 1) ^ (- int64_t(x >> 63)));
    }

    void write_bytearray(string_view value)
    {
        write_varint(value.size());
        auto old_ptr = advance_ptr(value.size());
        memcpy(old_ptr, value.data(), value.size());
    }

    void write_field_tag(uint32_t field_num, WireType wire_type)
    {
        write_varint(field_num*8 + wire_type);
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
        auto start_pos = start_length_delimited();
        code();
        commit_length_delimited(start_pos);
    }

// Define put_map* method for map<TYPE1,TYPE2>
#define define_map_writer(TYPE1, TYPE2)                                                                            \
    template <typename FieldType>                                                                                  \
    void put_map_##TYPE1##_##TYPE2(uint32_t field_num, FieldType&& value)                                          \
    {                                                                                                              \
        for(const auto& x : value)                                                                                 \
        {                                                                                                          \
            write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);                                                 \
            write_length_delimited([&]{                                                                            \
                put_##TYPE1(1, x.first);                                                                           \
                put_##TYPE2(2, x.second);                                                                          \
            });                                                                                                    \
        }                                                                                                          \
    }                                                                                                              \

// Define put_* methods for TYPE and put_map* methods for any map<TYPE,*>
#define define_writers(TYPE, C_TYPE, WIRETYPE, WRITER)                                                             \
                                                                                                                   \
    void put_##TYPE(uint32_t field_num, C_TYPE value)                                                              \
    {                                                                                                              \
        write_field_tag(field_num, WIRETYPE);                                                                      \
        WRITER(value);                                                                                             \
    }                                                                                                              \
                                                                                                                   \
    template <typename FieldType>                                                                                  \
    void put_repeated_##TYPE(uint32_t field_num, FieldType&& value)                                                \
    {                                                                                                              \
        for(auto &x: value)  put_##TYPE(field_num, x);                                                             \
    }                                                                                                              \
                                                                                                                   \
    template <typename FieldType>                                                                                  \
    void put_packed_##TYPE(uint32_t field_num, FieldType&& value)                                                  \
    {                                                                                                              \
        static_assert(std::is_scalar<C_TYPE>(),                                                                    \
            "put_packed_" #TYPE " isn't defined according to ProtoBuf format specifications");                     \
                                                                                                                   \
        write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);                                                     \
        write_length_delimited([&]{ for(auto &x: value)  WRITER(x); });                                            \
    }                                                                                                              \
                                                                                                                   \
    define_map_writer(TYPE, int32)                                                                                 \
    define_map_writer(TYPE, int64)                                                                                 \
    define_map_writer(TYPE, uint32)                                                                                \
    define_map_writer(TYPE, uint64)                                                                                \
                                                                                                                   \
    define_map_writer(TYPE, sfixed32)                                                                              \
    define_map_writer(TYPE, sfixed64)                                                                              \
    define_map_writer(TYPE, fixed32)                                                                               \
    define_map_writer(TYPE, fixed64)                                                                               \
                                                                                                                   \
    define_map_writer(TYPE, sint32)                                                                                \
    define_map_writer(TYPE, sint64)                                                                                \
                                                                                                                   \
    define_map_writer(TYPE, bool)                                                                                  \
    define_map_writer(TYPE, enum)                                                                                  \
                                                                                                                   \
    define_map_writer(TYPE, float)                                                                                 \
    define_map_writer(TYPE, double)                                                                                \
                                                                                                                   \
    define_map_writer(TYPE, string)                                                                                \
    define_map_writer(TYPE, bytes)                                                                                 \
                                                                                                                   \
    define_map_writer(TYPE, message)                                                                               \


    define_writers(int32, int32_t, WIRETYPE_VARINT, write_varint)
    define_writers(int64, int64_t, WIRETYPE_VARINT, write_varint)
    define_writers(uint32, uint32_t, WIRETYPE_VARINT, write_varint)
    define_writers(uint64, uint64_t, WIRETYPE_VARINT, write_varint)

    define_writers(sfixed32, int32_t, WIRETYPE_FIXED32, write_fixed_width)
    define_writers(sfixed64, int64_t, WIRETYPE_FIXED64, write_fixed_width)
    define_writers(fixed32, uint32_t, WIRETYPE_FIXED32, write_fixed_width)
    define_writers(fixed64, uint64_t, WIRETYPE_FIXED64, write_fixed_width)

    define_writers(sint32, int32_t, WIRETYPE_VARINT, write_zigzag)
    define_writers(sint64, int64_t, WIRETYPE_VARINT, write_zigzag)

    define_writers(bool, bool, WIRETYPE_VARINT, write_varint)
    define_writers(enum, int32_t, WIRETYPE_VARINT, write_varint)

    define_writers(float, float, WIRETYPE_FIXED32, write_fixed_width)
    define_writers(double, double, WIRETYPE_FIXED64, write_fixed_width)

    define_writers(string, string_view, WIRETYPE_LENGTH_DELIMITED, write_bytearray)
    define_writers(bytes, string_view, WIRETYPE_LENGTH_DELIMITED, write_bytearray)

#undef define_map_writer
#undef define_writers

    template <typename FieldType>
    void put_message(uint32_t field_num, FieldType&& value)
    {
        write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
        write_length_delimited([&]{ value.encode(*this); });
    }

    template <typename FieldType>
    void put_repeated_message(uint32_t field_num, FieldType&& value)
    {
        for(auto &x: value)  put_message(field_num, x);
    }
};


template <typename MessageType>
inline std::string encode(MessageType&& msg)
{
    Encoder pb;
    msg.encode(pb);
    return pb.result();
}



/*
Decoder library consists of 3 levels:
- 1st level defines read_varint() and read_fixed_width(), allowing to grab basic values from input buffer
- 2nd level defines parse_*_value(), allowing to read a field knowing field's type and wiretype
- 3rd level defines get_*(), providing easy-to-use API for users of this class
*/
struct Decoder;

template <typename MessageType>
inline MessageType decode(string_view buffer)
{
    Decoder pb(buffer);
    MessageType msg;
    msg.decode(pb);
    return msg;
}


struct Decoder
{
    const char* ptr = nullptr;
    const char* buf_end = nullptr;
    uint32_t field_num = -1;
    WireType wire_type = WIRETYPE_UNDEFINED;


    explicit Decoder(string_view view) noexcept
        : ptr     {view.data()},
          buf_end {view.data() + view.size()}
    {
    }

    void advance_ptr(int bytes)
    {
        if(buf_end - ptr < bytes)  throw std::runtime_error("Unexpected end of buffer");
        ptr += bytes;
    }

    bool eof()
    {
        return(ptr >= buf_end);
    }


    template <typename FixedType>
    FixedType read_fixed_width()
    {
        auto old_ptr = ptr;
        advance_ptr(sizeof(FixedType));
        return read_from_little_endian<FixedType>(old_ptr);
    }

    uint64_t read_varint_slow()
    {
        uint64_t value = 0;
        uint64_t byte;
        int shift = 0;

        do {
            if(eof())        throw std::runtime_error("Unexpected end of buffer in varint");
            if(shift >= 64)  throw std::runtime_error("More than 10 bytes in varint");

            byte = *(uint8_t*)ptr;
            value |= ((byte & 127) << shift);
            ptr++;  shift += 7;
        }
        while(byte & 128);

        return value;
    }

    uint64_t read_varint()
    {
        if(buf_end - ptr < 10)  return read_varint_slow();

        auto p = (uint8_t*)ptr;
        uint64_t value = 0;

#define STEP(n) \
    value |= (uint64_t(p[n] & 127) << (n*7)); \
    if(p[n] < 128)  {ptr += n + 1;  return value;}

        STEP(0)
        STEP(1)
        STEP(2)
        STEP(3)
        STEP(4)
        STEP(5)
        STEP(6)
        STEP(7)
        STEP(8)
        STEP(9)
#undef STEP
        throw std::runtime_error("More than 10 bytes in varint");
    }

    int64_t read_zigzag()
    {
        uint64_t value = read_varint();
        return (value >> 1) ^ (- int64_t(value & 1));
    }


    template <typename FloatingPointType>
    FloatingPointType parse_fp_value()
    {
        switch(wire_type) {
            case WIRETYPE_FIXED64: return read_fixed_width<double>();
            case WIRETYPE_FIXED32: return read_fixed_width<float>();
        }

        throw std::runtime_error("Can't parse floating-point value with field type " + std::to_string(wire_type));
    }

    uint64_t parse_integer_value()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_varint();
            case WIRETYPE_FIXED64:  return read_fixed_width<uint64_t>();
            case WIRETYPE_FIXED32:  return read_fixed_width<uint32_t>();
        }

        throw std::runtime_error("Can't parse integral value with field type " + std::to_string(wire_type));
    }

    int64_t parse_zigzag_value()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_zigzag();
            case WIRETYPE_FIXED64:  return read_fixed_width<int64_t>();
            case WIRETYPE_FIXED32:  return read_fixed_width<int32_t>();
        }

        throw std::runtime_error("Can't parse zigzag integral with field type " + std::to_string(wire_type));
    }

    string_view parse_bytearray_value()
    {
        if(wire_type != WIRETYPE_LENGTH_DELIMITED) {
            throw std::runtime_error("Can't parse bytearray with field type " + std::to_string(wire_type));
        }

        uint64_t len = read_varint();
        advance_ptr(len);

        return {ptr-len, len};
    }


    bool get_next_field()
    {
        if(eof())  return false;

        uint64_t number = read_varint();
        field_num = (number / 8);
        wire_type = WireType(number % 8);

        return true;
    }

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
            advance_ptr(len);
        } else {
            throw std::runtime_error("Unsupported field type " + std::to_string(wire_type));
        }
    }


// Define get_map* method for map<TYPE1,TYPE2>
#define define_map_reader(TYPE1, TYPE2)                                                                            \
    template <typename FieldType>                                                                                  \
    void get_map_##TYPE1##_##TYPE2(FieldType *field)                                                               \
    {                                                                                                              \
        Decoder sub_decoder(parse_bytearray_value());                                                              \
        bool has_key = false, has_value = false;                                                                   \
        typename FieldType::key_type key;                                                                          \
        typename FieldType::mapped_type value;                                                                     \
                                                                                                                   \
        while(sub_decoder.get_next_field())                                                                        \
        {                                                                                                          \
            switch(sub_decoder.field_num)                                                                          \
            {                                                                                                      \
                case 1: sub_decoder.get_##TYPE1(&key, &has_key); break;                                            \
                case 2: sub_decoder.get_##TYPE2(&value, &has_value); break;                                        \
                default: sub_decoder.skip_field();                                                                 \
            }                                                                                                      \
        }                                                                                                          \
                                                                                                                   \
        if (has_key && has_value) {                                                                                \
            (*field)[key] = value;                                                                                 \
        }                                                                                                          \
    }                                                                                                              \

#define define_readers(TYPE, C_TYPE, PARSER, READER)                                                               \
                                                                                                                   \
    C_TYPE get_##TYPE()                                                                                            \
    {                                                                                                              \
        using FieldType = C_TYPE;                                                                                  \
        return FieldType(PARSER());                                                                                \
    }                                                                                                              \
                                                                                                                   \
    template <typename FieldType>                                                                                  \
    void get_##TYPE(FieldType *field, bool *has_field = nullptr)                                                   \
    {                                                                                                              \
        *field = FieldType(PARSER());                                                                              \
        if(has_field)  *has_field = true;                                                                          \
    }                                                                                                              \
                                                                                                                   \
    template <typename RepeatedFieldType>                                                                          \
    void get_repeated_##TYPE(RepeatedFieldType *field)                                                             \
    {                                                                                                              \
        using FieldType = typename RepeatedFieldType::value_type;                                                  \
                                                                                                                   \
        if(std::is_scalar<C_TYPE>()  &&  (wire_type == WIRETYPE_LENGTH_DELIMITED)) {                               \
            /* Parsing packed repeated field */                                                                    \
            Decoder sub_decoder(parse_bytearray_value());                                                          \
            while(! sub_decoder.eof()) {                                                                           \
                field->push_back( FieldType(sub_decoder.READER()) );                                               \
            }                                                                                                      \
        } else {                                                                                                   \
            field->push_back( FieldType(PARSER()) );                                                               \
        }                                                                                                          \
    }                                                                                                              \
                                                                                                                   \
    define_map_reader(TYPE, int32)                                                                                 \
    define_map_reader(TYPE, int64)                                                                                 \
    define_map_reader(TYPE, uint32)                                                                                \
    define_map_reader(TYPE, uint64)                                                                                \
                                                                                                                   \
    define_map_reader(TYPE, sfixed32)                                                                              \
    define_map_reader(TYPE, sfixed64)                                                                              \
    define_map_reader(TYPE, fixed32)                                                                               \
    define_map_reader(TYPE, fixed64)                                                                               \
                                                                                                                   \
    define_map_reader(TYPE, sint32)                                                                                \
    define_map_reader(TYPE, sint64)                                                                                \
                                                                                                                   \
    define_map_reader(TYPE, bool)                                                                                  \
    define_map_reader(TYPE, enum)                                                                                  \
                                                                                                                   \
    define_map_reader(TYPE, float)                                                                                 \
    define_map_reader(TYPE, double)                                                                                \
                                                                                                                   \
    define_map_reader(TYPE, string)                                                                                \
    define_map_reader(TYPE, bytes)                                                                                 \
                                                                                                                   \
    define_map_reader(TYPE, message)                                                                               \


    define_readers(int32, int32_t, parse_integer_value, read_varint)
    define_readers(int64, int64_t, parse_integer_value, read_varint)
    define_readers(uint32, uint32_t, parse_integer_value, read_varint)
    define_readers(uint64, uint64_t, parse_integer_value, read_varint)

    define_readers(sfixed32, int32_t, parse_integer_value, read_fixed_width<int32_t>)
    define_readers(sfixed64, int64_t, parse_integer_value, read_fixed_width<int64_t>)
    define_readers(fixed32, uint32_t, parse_integer_value, read_fixed_width<uint32_t>)
    define_readers(fixed64, uint64_t, parse_integer_value, read_fixed_width<uint64_t>)

    define_readers(sint32, int32_t, parse_zigzag_value, read_zigzag)
    define_readers(sint64, int64_t, parse_zigzag_value, read_zigzag)

    define_readers(bool, bool, parse_integer_value, read_varint)
    define_readers(enum, int32_t, parse_integer_value, read_varint)

    define_readers(float, float, parse_fp_value<FieldType>, read_fixed_width<float>)
    define_readers(double, double, parse_fp_value<FieldType>, read_fixed_width<double>)

    define_readers(string, string_view, parse_bytearray_value, parse_bytearray_value)
    define_readers(bytes, string_view, parse_bytearray_value, parse_bytearray_value)

#undef define_map_reader
#undef define_readers

    template <typename MessageType>
    void get_message(MessageType *field, bool *has_field = nullptr)
    {
        Decoder pb(parse_bytearray_value());
        field->decode(pb);
        if(has_field)  *has_field = true;
    }

    template <typename RepeatedMessageType>
    void get_repeated_message(RepeatedMessageType *field)
    {
        using T = typename RepeatedMessageType::value_type;
        field->push_back( decode<T>(parse_bytearray_value()) );
    }
};

}  // namespace easypb
