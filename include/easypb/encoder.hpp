#pragma once
#include "common.hpp"


namespace easypb
{

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

}  // namespace easypb
