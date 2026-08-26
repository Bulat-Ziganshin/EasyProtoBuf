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
#include <type_traits>
#include <utility>
#include <vector>
#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#include <string_view>
#endif


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


// Protobuf field type used by the generic get/put API.
enum PBType
{
    PB_INT32,
    PB_INT64,
    PB_UINT32,
    PB_UINT64,
    PB_SFIXED32,
    PB_SFIXED64,
    PB_FIXED32,
    PB_FIXED64,
    PB_SINT32,
    PB_SINT64,
    PB_BOOL,
    PB_ENUM,
    PB_FLOAT,
    PB_DOUBLE,
    PB_STRING,
    PB_BYTES,
    PB_MESSAGE,
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
// Low-level writer for the Protobuf wire format
// ****************************************************************************
struct Writer
{
    // Invariants:
    //   buf_end == buffer.data() + buffer.size()
    //   buffer.data() <= ptr <= buf_end

    std::string buffer; // buffer storing the serialized data
    char* ptr;          // the current writing point
    char* buf_end;      // end of the allocated space
    char* begin() const {return (char*)(buffer.data());}  // start of the allocated space
    size_t pos()  const {return ptr - begin();}           // the current writing index


    Writer()
    {
        ptr = buf_end = begin();
    }

    // Return the collected buffer and start from scratch
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
};

/*****************************************************************************
Low-level reader for the Protobuf wire format.
*****************************************************************************/
struct Reader
{
    // Invariants:
    //   ptr <= buf_end

    // The bytes between ptr and buf_end contain the not-yet-decoded remainder of the message.
    const char* ptr = nullptr;
    const char* buf_end = nullptr;

    // These properties are filled by get_next_field() and make sense only till the entire field is decoded
    uint32_t field_num = UINT32_MAX;
    WireType wire_type = WIRETYPE_UNDEFINED;


    // Reader keeps pointers into the data being decoded, so don't free/move them till the decoding is finished
    explicit Reader(const char* buffer, size_t size) noexcept
        : ptr{buffer}, buf_end{buffer + size}
    {
    }

    explicit Reader(string_view view) noexcept
        : Reader(view.data(), view.size())
    {
    }

    // Prohibit Reader(std::string_view(char*)), since it creates a Decoder with an incorrect bufsize
    explicit Reader(const char*) = delete;


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

    // A sint32 value truncates the raw varint before applying ZigZag decoding.
    int32_t read_zigzag32()
    {
        uint32_t value = uint32_t(read_varint());
        return int32_t((value >> 1) ^ uint32_t(- int32_t(value & 1)));
    }

    // Read zigzag-encoded integer value
    int64_t read_zigzag64()
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

    int32_t parse_zigzag32_value()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_zigzag32();
            case WIRETYPE_FIXED64:  return int32_t(read_fixed_width<int64_t>());
            case WIRETYPE_FIXED32:  return read_fixed_width<int32_t>();
            default:                throw wiretype_mismatch("Can't parse zigzag integral value with wiretype "
                                            + std::to_string(wire_type));
        }
    }

    int64_t parse_zigzag_value64()
    {
        switch(wire_type) {
            case WIRETYPE_VARINT:   return read_zigzag64();
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
        } else if (wire_type == WIRETYPE_START_GROUP) {
            // Keep an explicit stack so deeply nested unknown groups do not
            // consume the C++ call stack while they are being skipped.
            std::vector<uint32_t> open_groups(1, field_num);
            while (!open_groups.empty()) {
                if (!get_next_field()) {
                    throw unexpected_eof("Unexpected end of buffer in group field "
                                         + std::to_string(open_groups.back()));
                }

                if (wire_type == WIRETYPE_START_GROUP) {
                    open_groups.push_back(field_num);
                } else if (wire_type == WIRETYPE_END_GROUP) {
                    if (field_num != open_groups.back()) {
                        throw wiretype_mismatch("Group field "
                            + std::to_string(open_groups.back())
                            + " ended by field " + std::to_string(field_num));
                    }
                    open_groups.pop_back();
                } else {
                    skip_field();
                }
            }
        } else {
            throw unsupported_wiretype("Unsupported wire type " + std::to_string(wire_type));
        }
    }
};


template <typename T>
struct dependent_false : std::false_type {};


// Low-level protobuf field encodings. These classes know only how to read and
// write a payload; scalar_pb_codec adds field tags, conversions, packed fields,
// and repeated-field handling.
template <typename CType>
struct integer_pb_field
{
    static void write(Writer& pb, CType value) { pb.write_varint(value); }
    static CType parse(Reader& pb) { return CType(pb.parse_integer_value()); }
    static CType read(Reader& pb) { return CType(pb.read_varint()); }
};


template <typename CType>
struct fixed_pb_field
{
    static void write(Writer& pb, CType value) { pb.write_fixed_width(value); }
    static CType parse(Reader& pb) { return CType(pb.parse_integer_value()); }
    static CType read(Reader& pb) { return pb.template read_fixed_width<CType>(); }
};


template <typename CType>
struct zigzag_pb_field;


template <>
struct zigzag_pb_field<int32_t>
{
    static void write(Writer& pb, int32_t value) { pb.write_zigzag(value); }
    static int32_t parse(Reader& pb) { return pb.parse_zigzag32_value(); }
    static int32_t read(Reader& pb) { return pb.read_zigzag32(); }
};


template <>
struct zigzag_pb_field<int64_t>
{
    static void write(Writer& pb, int64_t value) { pb.write_zigzag(value); }
    static int64_t parse(Reader& pb) { return pb.parse_zigzag_value64(); }
    static int64_t read(Reader& pb) { return pb.read_zigzag64(); }
};


template <typename CType>
struct floating_pb_field
{
    static void write(Writer& pb, CType value) { pb.write_fixed_width(value); }
    static CType parse(Reader& pb) { return pb.template parse_fp_value<CType>(); }
    static CType read(Reader& pb) { return pb.template read_fixed_width<CType>(); }
};


// Common implementation for scalar protobuf types. Field supplies the raw
// payload read/write operations; CType and Wire describe protobuf semantics.
template <typename Field, typename CType, WireType Wire>
struct scalar_pb_codec
{
    typedef CType value_type;
    static const bool packable = true;

    template <typename FieldType>
    static void put(Writer& pb, uint32_t field_num, const FieldType& value)
    {
        pb.write_field_tag(field_num, Wire);
        Field::write(pb, CType(value));
    }

    static CType get(Reader& pb)
    {
        return Field::parse(pb);
    }

    template <typename FieldType>
    static void get(Reader& pb, FieldType* field, bool* has_field = nullptr)
    {
        *field = FieldType(Field::parse(pb));
        if (has_field)  *has_field = true;
    }

    template <typename FieldType>
    static void put_packed(Writer& pb, uint32_t field_num, const FieldType& value)
    {
        pb.write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
        pb.write_length_delimited([&]{
            for (const auto& x : value)  Field::write(pb, CType(x));
        });
    }

    template <typename RepeatedFieldType>
    static void get_repeated(Reader& pb, RepeatedFieldType* field)
    {
        typedef typename RepeatedFieldType::value_type FieldType;

        if (pb.wire_type == WIRETYPE_LENGTH_DELIMITED) {
            Reader sub_reader(pb.parse_bytearray_value());
            while (!sub_reader.eof()) {
                field->push_back(FieldType(Field::read(sub_reader)));
            }
        } else {
            field->push_back(FieldType(Field::parse(pb)));
        }
    }
};


struct bytearray_pb_codec
{
    typedef string_view value_type;
    static const bool packable = false;

    static void put(Writer& pb, uint32_t field_num, string_view value)
    {
        pb.write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
        pb.write_bytearray(value);
    }

    static string_view get(Reader& pb)
    {
        return pb.parse_bytearray_value();
    }

    template <typename FieldType>
    static void get(Reader& pb, FieldType* field, bool* has_field = nullptr)
    {
        *field = FieldType(pb.parse_bytearray_value());
        if (has_field)  *has_field = true;
    }

    template <typename FieldType>
    static void put_packed(Writer&, uint32_t, const FieldType&)
    {
        static_assert(dependent_false<FieldType>::value,
            "This protobuf type cannot be packed");
    }

    template <typename RepeatedFieldType>
    static void get_repeated(Reader& pb, RepeatedFieldType* field)
    {
        typedef typename RepeatedFieldType::value_type FieldType;
        field->push_back(FieldType(pb.parse_bytearray_value()));
    }
};


// Message is the only PBType whose C++ value_type is not fixed and whose
// codec needs the high-level Encoder/Decoder API for recursive customization.
struct message_pb_codec
{
    // A message has no fixed value type. The void alias lets the value-returning
    // Decoder::get<PB_MESSAGE>() overload produce a deliberate diagnostic.
    typedef void value_type;
    static const bool packable = false;

    template <typename EncoderType, typename FieldType>
    static void put(EncoderType& pb, uint32_t field_num, const FieldType& value)
    {
        pb.write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
        pb.write_length_delimited([&]{ encode(pb, value); });
    }

    template <typename DecoderType, typename FieldType>
    static void get(DecoderType& pb, FieldType* field, bool* has_field = nullptr)
    {
        decode(DecoderType(pb.parse_bytearray_value()), *field);
        if (has_field)  *has_field = true;
    }

    template <typename DecoderType>
    static void get(DecoderType&)
    {
        static_assert(dependent_false<DecoderType>::value,
            "PB_MESSAGE requires an output object");
    }

    template <typename FieldType>
    static void put_packed(Writer&, uint32_t, const FieldType&)
    {
        static_assert(dependent_false<FieldType>::value,
            "PB_MESSAGE cannot be packed");
    }

    template <typename DecoderType, typename RepeatedFieldType>
    static void get_repeated(DecoderType& pb, RepeatedFieldType* field)
    {
        typedef typename RepeatedFieldType::value_type FieldType;
        FieldType value{};
        decode(DecoderType(pb.parse_bytearray_value()), value);
        field->push_back(std::move(value));
    }
};


// Compile-time dispatcher from protobuf type to codec.
template <PBType Type>
struct pb_type_codec;


template <> struct pb_type_codec<PB_INT32>    : scalar_pb_codec<integer_pb_field<int32_t>, int32_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_INT64>    : scalar_pb_codec<integer_pb_field<int64_t>, int64_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_UINT32>   : scalar_pb_codec<integer_pb_field<uint32_t>, uint32_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_UINT64>   : scalar_pb_codec<integer_pb_field<uint64_t>, uint64_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_SFIXED32> : scalar_pb_codec<fixed_pb_field<int32_t>, int32_t, WIRETYPE_FIXED32> {};
template <> struct pb_type_codec<PB_SFIXED64> : scalar_pb_codec<fixed_pb_field<int64_t>, int64_t, WIRETYPE_FIXED64> {};
template <> struct pb_type_codec<PB_FIXED32>  : scalar_pb_codec<fixed_pb_field<uint32_t>, uint32_t, WIRETYPE_FIXED32> {};
template <> struct pb_type_codec<PB_FIXED64>  : scalar_pb_codec<fixed_pb_field<uint64_t>, uint64_t, WIRETYPE_FIXED64> {};
template <> struct pb_type_codec<PB_SINT32>   : scalar_pb_codec<zigzag_pb_field<int32_t>, int32_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_SINT64>   : scalar_pb_codec<zigzag_pb_field<int64_t>, int64_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_BOOL>     : scalar_pb_codec<integer_pb_field<bool>, bool, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_ENUM>     : scalar_pb_codec<integer_pb_field<int32_t>, int32_t, WIRETYPE_VARINT> {};
template <> struct pb_type_codec<PB_FLOAT>    : scalar_pb_codec<floating_pb_field<float>, float, WIRETYPE_FIXED32> {};
template <> struct pb_type_codec<PB_DOUBLE>   : scalar_pb_codec<floating_pb_field<double>, double, WIRETYPE_FIXED64> {};
template <> struct pb_type_codec<PB_STRING>   : bytearray_pb_codec {};
template <> struct pb_type_codec<PB_BYTES>    : bytearray_pb_codec {};
template <> struct pb_type_codec<PB_MESSAGE>  : message_pb_codec {};


// Protobuf permits only integral, bool, and string map keys.
template <PBType Type>
struct is_valid_map_key_type : std::integral_constant<bool,
    Type == PB_INT32 || Type == PB_INT64 ||
    Type == PB_UINT32 || Type == PB_UINT64 ||
    Type == PB_SFIXED32 || Type == PB_SFIXED64 ||
    Type == PB_FIXED32 || Type == PB_FIXED64 ||
    Type == PB_SINT32 || Type == PB_SINT64 ||
    Type == PB_BOOL || Type == PB_STRING> {};


/*****************************************************************************
High-level encoder API.
*****************************************************************************/
struct Encoder : Writer
{
    Encoder() : Writer() {}

    // Generic compile-time API.
    template <PBType Type, typename FieldType>
    void put(uint32_t field_num, const FieldType& value)
    {
        pb_type_codec<Type>::put(*this, field_num, value);
    }

    template <PBType Type, typename FieldType>
    void put_repeated(uint32_t field_num, const FieldType& value)
    {
        for (const auto& x : value)  put<Type>(field_num, x);
    }

    template <PBType Type, typename FieldType>
    void put_packed(uint32_t field_num, const FieldType& value)
    {
        pb_type_codec<Type>::put_packed(*this, field_num, value);
    }

    template <PBType KeyType, PBType ValueType, typename FieldType>
    void put_map(uint32_t field_num, const FieldType& value)
    {
        static_assert(is_valid_map_key_type<KeyType>::value,
            "Invalid protobuf map key type");

        for (const auto& x : value)
        {
            write_field_tag(field_num, WIRETYPE_LENGTH_DELIMITED);
            write_length_delimited([&]{
                put<KeyType>(1, x.first);
                put<ValueType>(2, x.second);
            });
        }
    }

    template <typename FieldType>
    void put_message(uint32_t field_num, const FieldType& value)
    { put<PB_MESSAGE>(field_num, value); }

    template <typename FieldType>
    void put_repeated_message(uint32_t field_num, const FieldType& value)
    { put_repeated<PB_MESSAGE>(field_num, value); }

// Define put_map_* wrapper for map<TYPE1,TYPE2>.
#define EASYPB_DEFINE_MAP_WRITER(TYPE1, PBTYPE1, TYPE2, PBTYPE2)              \
    template <typename FieldType>                                             \
    void put_map_##TYPE1##_##TYPE2(uint32_t field_num, const FieldType& value)\
    { put_map<PBTYPE1, PBTYPE2>(field_num, value); }                          \
/* end of EASYPB_DEFINE_MAP_WRITER macro definition */

// Define all map wrappers whose key has TYPE/PBTYPE.
#define EASYPB_DEFINE_MAP_WRITERS(TYPE, PBTYPE)                               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, int32,    PB_INT32)                \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, int64,    PB_INT64)                \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, uint32,   PB_UINT32)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, uint64,   PB_UINT64)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, sfixed32, PB_SFIXED32)             \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, sfixed64, PB_SFIXED64)             \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, fixed32,  PB_FIXED32)              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, fixed64,  PB_FIXED64)              \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, sint32,   PB_SINT32)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, sint64,   PB_SINT64)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, bool,     PB_BOOL)                 \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, enum,     PB_ENUM)                 \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, float,    PB_FLOAT)                \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, double,   PB_DOUBLE)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, string,   PB_STRING)               \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, bytes,    PB_BYTES)                \
    EASYPB_DEFINE_MAP_WRITER(TYPE, PBTYPE, message,  PB_MESSAGE)              \
/* end of EASYPB_DEFINE_MAP_WRITERS macro definition */

// Define named wrappers for one protobuf type.
#define EASYPB_DEFINE_WRITERS(TYPE, PBTYPE)                                   \
    template <typename FieldType>                                             \
    void put_##TYPE(uint32_t field_num, const FieldType& value)               \
    { put<PBTYPE>(field_num, value); }                                        \
                                                                              \
    template <typename FieldType>                                             \
    void put_repeated_##TYPE(uint32_t field_num, const FieldType& value)      \
    { put_repeated<PBTYPE>(field_num, value); }                               \
                                                                              \
    template <typename FieldType>                                             \
    void put_packed_##TYPE(uint32_t field_num, const FieldType& value)        \
    { put_packed<PBTYPE>(field_num, value); }                                 \
                                                                              \
    EASYPB_DEFINE_MAP_WRITERS(TYPE, PBTYPE)                                   \
/* end of EASYPB_DEFINE_WRITERS macro definition */

    EASYPB_DEFINE_WRITERS(int32,    PB_INT32)
    EASYPB_DEFINE_WRITERS(int64,    PB_INT64)
    EASYPB_DEFINE_WRITERS(uint32,   PB_UINT32)
    EASYPB_DEFINE_WRITERS(uint64,   PB_UINT64)
    EASYPB_DEFINE_WRITERS(sfixed32, PB_SFIXED32)
    EASYPB_DEFINE_WRITERS(sfixed64, PB_SFIXED64)
    EASYPB_DEFINE_WRITERS(fixed32,  PB_FIXED32)
    EASYPB_DEFINE_WRITERS(fixed64,  PB_FIXED64)
    EASYPB_DEFINE_WRITERS(sint32,   PB_SINT32)
    EASYPB_DEFINE_WRITERS(sint64,   PB_SINT64)
    EASYPB_DEFINE_WRITERS(bool,     PB_BOOL)
    EASYPB_DEFINE_WRITERS(enum,     PB_ENUM)
    EASYPB_DEFINE_WRITERS(float,    PB_FLOAT)
    EASYPB_DEFINE_WRITERS(double,   PB_DOUBLE)
    EASYPB_DEFINE_WRITERS(string,   PB_STRING)
    EASYPB_DEFINE_WRITERS(bytes,    PB_BYTES)

#undef EASYPB_DEFINE_MAP_WRITER
#undef EASYPB_DEFINE_MAP_WRITERS
#undef EASYPB_DEFINE_WRITERS

};


// Message customization protocol:
//   void encode(Encoder&, const T&);
// The call below is intentionally unqualified, so argument-dependent lookup
// finds an overload beside T or in namespace easypb.
template <typename MessageType>
inline std::string encode(const MessageType& msg)
{
    Encoder pb;
    encode(pb, msg);
    return pb.result();
}


/*****************************************************************************
High-level decoder API.
*****************************************************************************/
struct Decoder : Reader
{
    explicit Decoder(const char* buffer, size_t size) noexcept
        : Reader(buffer, size)
    {
    }

    explicit Decoder(string_view view) noexcept
        : Reader(view)
    {
    }

    explicit Decoder(const char*) = delete;

    // Generic compile-time API. get<Type>() is available for PBTypes with a
    // fixed C++ value_type; PB_MESSAGE is decoded through get<PB_MESSAGE>(field).
    template <PBType Type>
    typename pb_type_codec<Type>::value_type get()
    {
        return pb_type_codec<Type>::get(*this);
    }

    template <PBType Type, typename FieldType>
    void get(FieldType* field, bool* has_field = nullptr)
    {
        pb_type_codec<Type>::get(*this, field, has_field);
    }

    template <PBType Type, typename RepeatedFieldType>
    void get_repeated(RepeatedFieldType* field)
    {
        pb_type_codec<Type>::get_repeated(*this, field);
    }

    template <PBType KeyType, PBType ValueType, typename FieldType>
    void get_map(FieldType* field)
    {
        static_assert(is_valid_map_key_type<KeyType>::value,
            "Invalid protobuf map key type");

        Decoder sub_decoder(parse_bytearray_value());
        typename FieldType::key_type key{};
        typename FieldType::mapped_type value{};

        while (sub_decoder.get_next_field())
        {
            switch (sub_decoder.field_num)
            {
                case 1: sub_decoder.get<KeyType>(&key); break;
                case 2: sub_decoder.get<ValueType>(&value); break;
                default: sub_decoder.skip_field();
            }
        }

        (*field)[std::move(key)] = std::move(value);
    }

    template <typename MessageType>
    void get_message(MessageType* field, bool* has_field = nullptr)
    { get<PB_MESSAGE>(field, has_field); }

    template <typename RepeatedMessageType>
    void get_repeated_message(RepeatedMessageType* field)
    { get_repeated<PB_MESSAGE>(field); }

// Define get_map_* wrapper for map<TYPE1,TYPE2>.
#define EASYPB_DEFINE_MAP_READER(TYPE1, PBTYPE1, TYPE2, PBTYPE2)              \
    template <typename FieldType>                                             \
    void get_map_##TYPE1##_##TYPE2(FieldType* field)                          \
    { get_map<PBTYPE1, PBTYPE2>(field); }                                     \
/* end of EASYPB_DEFINE_MAP_READER macro definition */

// Define all map wrappers whose key has TYPE/PBTYPE.
#define EASYPB_DEFINE_MAP_READERS(TYPE, PBTYPE)                               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, int32,    PB_INT32)                \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, int64,    PB_INT64)                \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, uint32,   PB_UINT32)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, uint64,   PB_UINT64)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, sfixed32, PB_SFIXED32)             \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, sfixed64, PB_SFIXED64)             \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, fixed32,  PB_FIXED32)              \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, fixed64,  PB_FIXED64)              \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, sint32,   PB_SINT32)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, sint64,   PB_SINT64)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, bool,     PB_BOOL)                 \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, enum,     PB_ENUM)                 \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, float,    PB_FLOAT)                \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, double,   PB_DOUBLE)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, string,   PB_STRING)               \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, bytes,    PB_BYTES)                \
    EASYPB_DEFINE_MAP_READER(TYPE, PBTYPE, message,  PB_MESSAGE)              \
/* end of EASYPB_DEFINE_MAP_READERS macro definition */

// Define named wrappers for one protobuf type.
#define EASYPB_DEFINE_READERS(TYPE, PBTYPE, CTYPE)                            \
    CTYPE get_##TYPE()                                                        \
    { return get<PBTYPE>(); }                                                 \
                                                                              \
    template <typename FieldType>                                             \
    void get_##TYPE(FieldType* field, bool* has_field = nullptr)              \
    { get<PBTYPE>(field, has_field); }                                        \
                                                                              \
    template <typename RepeatedFieldType>                                     \
    void get_repeated_##TYPE(RepeatedFieldType* field)                        \
    { get_repeated<PBTYPE>(field); }                                          \
                                                                              \
    EASYPB_DEFINE_MAP_READERS(TYPE, PBTYPE)                                   \
/* end of EASYPB_DEFINE_READERS macro definition */

    EASYPB_DEFINE_READERS(int32,    PB_INT32,    int32_t)
    EASYPB_DEFINE_READERS(int64,    PB_INT64,    int64_t)
    EASYPB_DEFINE_READERS(uint32,   PB_UINT32,   uint32_t)
    EASYPB_DEFINE_READERS(uint64,   PB_UINT64,   uint64_t)
    EASYPB_DEFINE_READERS(sfixed32, PB_SFIXED32, int32_t)
    EASYPB_DEFINE_READERS(sfixed64, PB_SFIXED64, int64_t)
    EASYPB_DEFINE_READERS(fixed32,  PB_FIXED32,  uint32_t)
    EASYPB_DEFINE_READERS(fixed64,  PB_FIXED64,  uint64_t)
    EASYPB_DEFINE_READERS(sint32,   PB_SINT32,   int32_t)
    EASYPB_DEFINE_READERS(sint64,   PB_SINT64,   int64_t)
    EASYPB_DEFINE_READERS(bool,     PB_BOOL,     bool)
    EASYPB_DEFINE_READERS(enum,     PB_ENUM,     int32_t)
    EASYPB_DEFINE_READERS(float,    PB_FLOAT,    float)
    EASYPB_DEFINE_READERS(double,   PB_DOUBLE,   double)
    EASYPB_DEFINE_READERS(string,   PB_STRING,   string_view)
    EASYPB_DEFINE_READERS(bytes,    PB_BYTES,    string_view)

#undef EASYPB_DEFINE_MAP_READER
#undef EASYPB_DEFINE_MAP_READERS
#undef EASYPB_DEFINE_READERS

};


// Matching decoding customization protocol:
//   void decode(Decoder, T&);
// Decoder is a cheap non-owning cursor and is deliberately passed by value.
template <typename MessageType>
inline MessageType decode(string_view buffer)
{
    MessageType msg{};
    decode(Decoder(buffer), msg);
    return msg;
}

}  // namespace easypb
