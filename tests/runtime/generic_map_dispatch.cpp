#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>

#include <easypb.hpp>


namespace
{

int failures = 0;

enum class MapEnum : int32_t
{
    value = 150,
};

struct MapMessage
{
    int32_t value = 0;
};

struct MoveOnlyMessage
{
    MoveOnlyMessage() : value(0) {}
    MoveOnlyMessage(MoveOnlyMessage&& other) noexcept : value(other.value) {}

    MoveOnlyMessage& operator=(MoveOnlyMessage&& other) noexcept
    {
        value = other.value;
        return *this;
    }

    MoveOnlyMessage(const MoveOnlyMessage&) = delete;
    MoveOnlyMessage& operator=(const MoveOnlyMessage&) = delete;

    int32_t value;
};


void encode(easypb::Encoder& pb, const MapMessage& value)
{
    pb.put_int32(1, value.value);
}


void decode(easypb::Decoder pb, MapMessage& value)
{
    while (pb.get_next_field()) {
        if (pb.field_num == 1) {
            pb.get_int32(&value.value);
        } else {
            pb.skip_field();
        }
    }
}


void encode(easypb::Encoder& pb, const MoveOnlyMessage& value)
{
    pb.put_int32(1, value.value);
}


void decode(easypb::Decoder pb, MoveOnlyMessage& value)
{
    while (pb.get_next_field()) {
        if (pb.field_num == 1) {
            pb.get_int32(&value.value);
        } else {
            pb.skip_field();
        }
    }
}


// Build byte-exact fixtures without using the encoder under test.
std::string bytes(std::initializer_list<unsigned int> values)
{
    std::string result;
    result.reserve(values.size());
    for (std::initializer_list<unsigned int>::const_iterator it = values.begin();
         it != values.end(); ++it)
    {
        result.push_back(static_cast<char>(*it));
    }
    return result;
}


void expect(bool condition, const char* test_name)
{
    if (!condition) {
        std::cerr << test_name << '\n';
        ++failures;
    }
}


void expect_map_dispatch(const std::string& generic,
                         const std::string& named,
                         const std::string& expected,
                         const char* test_name)
{
    expect(generic == expected, test_name);
    expect(named == expected, test_name);
}


void test_every_valid_map_key_dispatch()
{
    {
        std::map<int32_t, int32_t> value;
        value[-1] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_INT32>(1, value);
        named.put_map_int32_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8d, 0x80, 0x80, 0x80, 0x00,
                   0x08,
                   0xff, 0xff, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff, 0x01,
                   0x10, 0x01}),
            "PB_INT32 map key dispatch");
    }

    {
        std::map<int64_t, int32_t> value;
        value[-1099511627776LL] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT64, easypb::PB_INT32>(1, value);
        named.put_map_int64_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8d, 0x80, 0x80, 0x80, 0x00,
                   0x08,
                   0x80, 0x80, 0x80, 0x80, 0x80,
                   0xe0, 0xff, 0xff, 0xff, 0x01,
                   0x10, 0x01}),
            "PB_INT64 map key dispatch");
    }

    {
        std::map<uint32_t, int32_t> value;
        value[UINT32_C(0xf1234567)] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_UINT32, easypb::PB_INT32>(1, value);
        named.put_map_uint32_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x88, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0xe7, 0x8a, 0x8d, 0x89, 0x0f,
                   0x10, 0x01}),
            "PB_UINT32 map key dispatch");
    }

    {
        std::map<uint64_t, int32_t> value;
        value[(UINT64_C(1) << 40) + 5] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_UINT64, easypb::PB_INT32>(1, value);
        named.put_map_uint64_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x89, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x85, 0x80, 0x80, 0x80, 0x80, 0x20,
                   0x10, 0x01}),
            "PB_UINT64 map key dispatch");
    }

    {
        std::map<int32_t, int32_t> value;
        value[-2] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_SFIXED32, easypb::PB_INT32>(1, value);
        named.put_map_sfixed32_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x87, 0x80, 0x80, 0x80, 0x00,
                   0x0d, 0xfe, 0xff, 0xff, 0xff,
                   0x10, 0x01}),
            "PB_SFIXED32 map key dispatch");
    }

    {
        std::map<int64_t, int32_t> value;
        value[-3] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_SFIXED64, easypb::PB_INT32>(1, value);
        named.put_map_sfixed64_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x09, 0xfd, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff,
                   0x10, 0x01}),
            "PB_SFIXED64 map key dispatch");
    }

    {
        std::map<uint32_t, int32_t> value;
        value[UINT32_C(0x89abcdef)] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_FIXED32, easypb::PB_INT32>(1, value);
        named.put_map_fixed32_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x87, 0x80, 0x80, 0x80, 0x00,
                   0x0d, 0xef, 0xcd, 0xab, 0x89,
                   0x10, 0x01}),
            "PB_FIXED32 map key dispatch");
    }

    {
        std::map<uint64_t, int32_t> value;
        value[UINT64_C(0x0123456789abcdef)] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_FIXED64, easypb::PB_INT32>(1, value);
        named.put_map_fixed64_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x09, 0xef, 0xcd, 0xab, 0x89,
                   0x67, 0x45, 0x23, 0x01,
                   0x10, 0x01}),
            "PB_FIXED64 map key dispatch");
    }

    {
        std::map<int32_t, int32_t> value;
        value[-2] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_SINT32, easypb::PB_INT32>(1, value);
        named.put_map_sint32_int32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x84, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x03, 0x10, 0x01}),
                            "PB_SINT32 map key dispatch");
    }

    {
        std::map<int64_t, int32_t> value;
        value[-1099511627776LL] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_SINT64, easypb::PB_INT32>(1, value);
        named.put_map_sint64_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x89, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f,
                   0x10, 0x01}),
            "PB_SINT64 map key dispatch");
    }

    {
        std::map<bool, int32_t> value;
        value[true] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_BOOL, easypb::PB_INT32>(1, value);
        named.put_map_bool_int32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x84, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01, 0x10, 0x01}),
                            "PB_BOOL map key dispatch");
    }

    {
        std::map<std::string, int32_t> value;
        value["k"] = 1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_STRING, easypb::PB_INT32>(1, value);
        named.put_map_string_int32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x85, 0x80, 0x80, 0x80, 0x00,
                                   0x0a, 0x01, 0x6b, 0x10, 0x01}),
                            "PB_STRING map key dispatch");
    }
}


void test_every_map_value_dispatch()
{
    {
        std::map<int32_t, int32_t> value;
        value[1] = -1;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_INT32>(1, value);
        named.put_map_int32_int32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8d, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01, 0x10,
                   0xff, 0xff, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff, 0x01}),
            "PB_INT32 map value dispatch");
    }

    {
        std::map<int32_t, int64_t> value;
        value[1] = -1099511627776LL;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_INT64>(1, value);
        named.put_map_int32_int64(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8d, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01, 0x10,
                   0x80, 0x80, 0x80, 0x80, 0x80,
                   0xe0, 0xff, 0xff, 0xff, 0x01}),
            "PB_INT64 map value dispatch");
    }

    {
        std::map<int32_t, uint32_t> value;
        value[1] = UINT32_C(0xf1234567);
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_UINT32>(1, value);
        named.put_map_int32_uint32(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x88, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x10, 0xe7, 0x8a, 0x8d, 0x89, 0x0f}),
            "PB_UINT32 map value dispatch");
    }

    {
        std::map<int32_t, uint64_t> value;
        value[1] = (UINT64_C(1) << 40) + 5;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_UINT64>(1, value);
        named.put_map_int32_uint64(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x89, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x10, 0x85, 0x80, 0x80, 0x80, 0x80, 0x20}),
            "PB_UINT64 map value dispatch");
    }

    {
        std::map<int32_t, int32_t> value;
        value[1] = -2;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_SFIXED32>(1, value);
        named.put_map_int32_sfixed32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x87, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01,
                                   0x15, 0xfe, 0xff, 0xff, 0xff}),
                            "PB_SFIXED32 map value dispatch");
    }

    {
        std::map<int32_t, int64_t> value;
        value[1] = -3;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_SFIXED64>(1, value);
        named.put_map_int32_sfixed64(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x11, 0xfd, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff}),
            "PB_SFIXED64 map value dispatch");
    }

    {
        std::map<int32_t, uint32_t> value;
        value[1] = UINT32_C(0x89abcdef);
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_FIXED32>(1, value);
        named.put_map_int32_fixed32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x87, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01,
                                   0x15, 0xef, 0xcd, 0xab, 0x89}),
                            "PB_FIXED32 map value dispatch");
    }

    {
        std::map<int32_t, uint64_t> value;
        value[1] = UINT64_C(0x0123456789abcdef);
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_FIXED64>(1, value);
        named.put_map_int32_fixed64(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x11, 0xef, 0xcd, 0xab, 0x89,
                   0x67, 0x45, 0x23, 0x01}),
            "PB_FIXED64 map value dispatch");
    }

    {
        std::map<int32_t, int32_t> value;
        value[1] = -2;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_SINT32>(1, value);
        named.put_map_int32_sint32(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x84, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01, 0x10, 0x03}),
                            "PB_SINT32 map value dispatch");
    }

    {
        std::map<int32_t, int64_t> value;
        value[1] = -1099511627776LL;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_SINT64>(1, value);
        named.put_map_int32_sint64(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x89, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f}),
            "PB_SINT64 map value dispatch");
    }

    {
        std::map<int32_t, bool> value;
        value[1] = true;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_BOOL>(1, value);
        named.put_map_int32_bool(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x84, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01, 0x10, 0x01}),
                            "PB_BOOL map value dispatch");
    }

    {
        std::map<int32_t, MapEnum> value;
        value[1] = MapEnum::value;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_ENUM>(1, value);
        named.put_map_int32_enum(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x85, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01, 0x10, 0x96, 0x01}),
                            "PB_ENUM map value dispatch");
    }

    {
        std::map<int32_t, float> value;
        value[1] = 1.5f;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_FLOAT>(1, value);
        named.put_map_int32_float(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x87, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01,
                                   0x15, 0x00, 0x00, 0xc0, 0x3f}),
                            "PB_FLOAT map value dispatch");
    }

    {
        std::map<int32_t, double> value;
        value[1] = -2.0;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_DOUBLE>(1, value);
        named.put_map_int32_double(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x11, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0xc0}),
            "PB_DOUBLE map value dispatch");
    }

    {
        std::map<int32_t, std::string> value;
        value[1] = "v";
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_STRING>(1, value);
        named.put_map_int32_string(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x85, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01,
                                   0x12, 0x01, 0x76}),
                            "PB_STRING map value dispatch");
    }

    {
        std::map<int32_t, std::string> value;
        value[1] = bytes({0x00, 0xff});
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_BYTES>(1, value);
        named.put_map_int32_bytes(1, value);
        expect_map_dispatch(generic.result(), named.result(),
                            bytes({0x0a, 0x86, 0x80, 0x80, 0x80, 0x00,
                                   0x08, 0x01,
                                   0x12, 0x02, 0x00, 0xff}),
                            "PB_BYTES map value dispatch");
    }

    {
        std::map<int32_t, MapMessage> value;
        value[1].value = 150;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_map<easypb::PB_INT32, easypb::PB_MESSAGE>(1, value);
        named.put_map_int32_message(1, value);
        expect_map_dispatch(
            generic.result(), named.result(),
            bytes({0x0a, 0x8b, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x01,
                   0x12, 0x83, 0x80, 0x80, 0x80, 0x00,
                   0x08, 0x96, 0x01}),
            "PB_MESSAGE map value dispatch");
    }
}


void test_map_decode_dispatch_from_independent_fixtures()
{
    {
        const std::string wire = bytes({
            0x0a, 0x0b,
            0x09, 0xfd, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff,
            0x10, 0x03,
        });
        std::map<int64_t, int32_t> generic;
        easypb::Decoder generic_decoder(wire.data(), wire.size());
        expect(generic_decoder.get_next_field(), "generic sfixed64 map field");
        generic_decoder.get_map<easypb::PB_SFIXED64, easypb::PB_INT32>(&generic);
        expect(generic.size() == 1 && generic.begin()->first == -3 &&
                   generic.begin()->second == 3,
               "generic sfixed64 map decode dispatch");

        std::map<int64_t, int32_t> named;
        easypb::Decoder named_decoder(wire.data(), wire.size());
        expect(named_decoder.get_next_field(), "named sfixed64 map field");
        named_decoder.get_map_sfixed64_int32(&named);
        expect(named == generic, "named sfixed64 map decode dispatch");
    }

    {
        const std::string wire = bytes({
            0x0a, 0x08,
            0x08, 0x01,
            0x12, 0x04, 0x00, 0xff, 0x41, 0x00,
        });
        std::map<bool, std::string> generic;
        easypb::Decoder generic_decoder(wire.data(), wire.size());
        expect(generic_decoder.get_next_field(), "generic bool-bytes map field");
        generic_decoder.get_map<easypb::PB_BOOL, easypb::PB_BYTES>(&generic);
        expect(generic.size() == 1 && generic.begin()->first &&
                   generic.begin()->second == bytes({0x00, 0xff, 0x41, 0x00}),
               "generic bool-bytes map decode dispatch");

        std::map<bool, std::string> named;
        easypb::Decoder named_decoder(wire.data(), wire.size());
        expect(named_decoder.get_next_field(), "named bool-bytes map field");
        named_decoder.get_map_bool_bytes(&named);
        expect(named == generic, "named bool-bytes map decode dispatch");
    }

    {
        const std::string wire = bytes({
            0x0a, 0x09,
            0x0a, 0x03, 0x6b, 0x65, 0x79,
            0x12, 0x02, 0x08, 0x09,
        });
        std::map<std::string, MapMessage> generic;
        easypb::Decoder generic_decoder(wire.data(), wire.size());
        expect(generic_decoder.get_next_field(), "generic string-message map field");
        generic_decoder.get_map<easypb::PB_STRING, easypb::PB_MESSAGE>(&generic);
        expect(generic.size() == 1 && generic.begin()->first == "key" &&
                   generic.begin()->second.value == 9,
               "generic string-message map decode dispatch");

        std::map<std::string, MapMessage> named;
        easypb::Decoder named_decoder(wire.data(), wire.size());
        expect(named_decoder.get_next_field(), "named string-message map field");
        named_decoder.get_map_string_message(&named);
        expect(named.size() == 1 && named.begin()->first == "key" &&
                   named.begin()->second.value == 9,
               "named string-message map decode dispatch");
    }

    {
        const std::string wire = bytes({
            0x0a, 0x05,
            0x08, 0x01,
            0x10, 0x96, 0x01,
        });
        std::map<int32_t, MapEnum> generic;
        easypb::Decoder generic_decoder(wire.data(), wire.size());
        expect(generic_decoder.get_next_field(), "generic enum map value field");
        generic_decoder.get_map<easypb::PB_INT32, easypb::PB_ENUM>(&generic);
        expect(generic.size() == 1 && generic.begin()->second == MapEnum::value,
               "generic enum map value decode dispatch");

        std::map<int32_t, MapEnum> named;
        easypb::Decoder named_decoder(wire.data(), wire.size());
        expect(named_decoder.get_next_field(), "named enum map value field");
        named_decoder.get_map_int32_enum(&named);
        expect(named == generic, "named enum map value decode dispatch");
    }
}


void test_move_only_message_map_value()
{
    std::map<int32_t, MoveOnlyMessage> source;
    source[7].value = 9;

    const std::string encoded = bytes({
        0x0a, 0x8a, 0x80, 0x80, 0x80, 0x00,
        0x08, 0x07,
        0x12, 0x82, 0x80, 0x80, 0x80, 0x00,
        0x08, 0x09,
    });
    easypb::Encoder generic_encoder;
    generic_encoder.put_map<easypb::PB_INT32, easypb::PB_MESSAGE>(1, source);
    expect(generic_encoder.result() == encoded,
           "generic map encode accepts a move-only message value");

    easypb::Encoder named_encoder;
    named_encoder.put_map_int32_message(1, source);
    expect(named_encoder.result() == encoded,
           "named map encode accepts a move-only message value");

    // Minimal lengths make this decoder fixture independent of the writer.
    const std::string wire = bytes({
        0x0a, 0x06,
        0x08, 0x07,
        0x12, 0x02, 0x08, 0x09,
    });
    std::map<int32_t, MoveOnlyMessage> decoded;
    easypb::Decoder decoder(wire.data(), wire.size());
    expect(decoder.get_next_field(), "move-only map value field");
    decoder.get_map<easypb::PB_INT32, easypb::PB_MESSAGE>(&decoded);
    expect(decoded.size() == 1 && decoded.begin()->first == 7 &&
               decoded.begin()->second.value == 9,
           "generic map decode moves a move-only message value");
}

}  // namespace


int main()
{
    test_every_valid_map_key_dispatch();
    test_every_map_value_dispatch();
    test_map_decode_dispatch_from_independent_fixtures();
    test_move_only_message_map_value();
    return failures == 0 ? 0 : 1;
}
