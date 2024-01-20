/*
Decoder library consists of 3 levels:
- 1st level defines read_varint() and read_fixed_width(), allowing to grab basic values from input buffer
- 2nd level defines parse_*_value(), allowing to read a field knowing field's type and wiretype
- 3rd level defines get_*(), providing easy-to-use API for users of this class
*/
#pragma once
#include "common.hpp"


namespace easypb
{

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
