#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include <easypb.hpp>


namespace
{

int failures = 0;

enum class DispatchEnum : int32_t
{
    zero = 0,
    value = 150,
};

struct Child
{
    int32_t value = 0;
};


void encode(easypb::Encoder& pb, const Child& child)
{
    pb.put_int32(1, child.value);
}


void decode(easypb::Decoder pb, Child& child)
{
    while (pb.get_next_field()) {
        if (pb.field_num == 1) {
            pb.get_int32(&child.value);
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


void expect_bytes(const std::string& actual, const std::string& expected,
                  const char* test_name)
{
    expect(actual == expected, test_name);
}


void expect_dispatch(const std::string& generic,
                     const std::string& named,
                     const std::string& expected,
                     const char* test_name)
{
    expect_bytes(generic, expected, test_name);
    expect_bytes(named, expected, test_name);
}


template <easypb::PBType Type, typename Expected, typename NamedGetter>
void expect_decode_dispatch(const std::string& wire,
                            const Expected& expected,
                            NamedGetter named_getter,
                            const char* test_name)
{
    easypb::Decoder generic_decoder(wire.data(), wire.size());
    if (!generic_decoder.get_next_field()) {
        expect(false, test_name);
        return;
    }

    expect(generic_decoder.field_num == 1, test_name);
    expect(generic_decoder.template get<Type>() == expected, test_name);
    expect(generic_decoder.eof(), test_name);

    easypb::Decoder named_decoder(wire.data(), wire.size());
    if (!named_decoder.get_next_field()) {
        expect(false, test_name);
        return;
    }

    expect(named_decoder.field_num == 1, test_name);
    expect(named_getter(named_decoder) == expected, test_name);
    expect(named_decoder.eof(), test_name);
}


void test_all_pb_types_encode_byte_exact_and_match_named_api()
{
    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_INT32>(1, int32_t(-1));
        named.put_int32(1, int32_t(-1));
        expect_dispatch(
            generic.result(), named.result(),
            bytes({0x08,
                   0xff, 0xff, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff, 0x01}),
            "PB_INT32 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const int64_t value = -1099511627776LL;
        generic.put<easypb::PB_INT64>(1, value);
        named.put_int64(1, value);
        expect_dispatch(
            generic.result(), named.result(),
            bytes({0x08,
                   0x80, 0x80, 0x80, 0x80, 0x80,
                   0xe0, 0xff, 0xff, 0xff, 0x01}),
            "PB_INT64 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const uint32_t value = UINT32_C(0xf1234567);
        generic.put<easypb::PB_UINT32>(1, value);
        named.put_uint32(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x08, 0xe7, 0x8a, 0x8d, 0x89, 0x0f}),
                        "PB_UINT32 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const uint64_t value = (UINT64_C(1) << 40) + 5;
        generic.put<easypb::PB_UINT64>(1, value);
        named.put_uint64(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x08, 0x85, 0x80, 0x80, 0x80, 0x80, 0x20}),
                        "PB_UINT64 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_SFIXED32>(1, int32_t(-2));
        named.put_sfixed32(1, int32_t(-2));
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0d, 0xfe, 0xff, 0xff, 0xff}),
                        "PB_SFIXED32 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_SFIXED64>(1, int64_t(-3));
        named.put_sfixed64(1, int64_t(-3));
        expect_dispatch(
            generic.result(), named.result(),
            bytes({0x09, 0xfd, 0xff, 0xff, 0xff,
                   0xff, 0xff, 0xff, 0xff}),
            "PB_SFIXED64 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const uint32_t value = UINT32_C(0x89abcdef);
        generic.put<easypb::PB_FIXED32>(1, value);
        named.put_fixed32(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0d, 0xef, 0xcd, 0xab, 0x89}),
                        "PB_FIXED32 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const uint64_t value = UINT64_C(0x0123456789abcdef);
        generic.put<easypb::PB_FIXED64>(1, value);
        named.put_fixed64(1, value);
        expect_dispatch(
            generic.result(), named.result(),
            bytes({0x09, 0xef, 0xcd, 0xab, 0x89,
                   0x67, 0x45, 0x23, 0x01}),
            "PB_FIXED64 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_SINT32>(1, int32_t(-2));
        named.put_sint32(1, int32_t(-2));
        expect_dispatch(generic.result(), named.result(), bytes({0x08, 0x03}),
                        "PB_SINT32 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        const int64_t value = -1099511627776LL;
        generic.put<easypb::PB_SINT64>(1, value);
        named.put_sint64(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f}),
                        "PB_SINT64 encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_BOOL>(1, 2);
        named.put_bool(1, 2);
        expect_dispatch(generic.result(), named.result(), bytes({0x08, 0x01}),
                        "PB_BOOL encode dispatch and normalization");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_ENUM>(1, DispatchEnum::value);
        named.put_enum(1, DispatchEnum::value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x08, 0x96, 0x01}),
                        "PB_ENUM encode dispatch");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_FLOAT>(1, 1.5);
        named.put_float(1, 1.5);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0d, 0x00, 0x00, 0xc0, 0x3f}),
                        "PB_FLOAT encode dispatch and narrowing");
    }

    {
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_DOUBLE>(1, -2.0);
        named.put_double(1, -2.0);
        expect_dispatch(
            generic.result(), named.result(),
            bytes({0x09, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0xc0}),
            "PB_DOUBLE encode dispatch");
    }

    {
        const std::string value = bytes({0x41, 0x00, 0x42});
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_STRING>(1, value);
        named.put_string(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0a, 0x03, 0x41, 0x00, 0x42}),
                        "PB_STRING encode dispatch");
    }

    {
        const std::string value = bytes({0x00, 0xff, 0x7f});
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_BYTES>(1, value);
        named.put_bytes(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0a, 0x03, 0x00, 0xff, 0x7f}),
                        "PB_BYTES encode dispatch");
    }

    {
        Child value;
        value.value = 150;
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put<easypb::PB_MESSAGE>(1, value);
        named.put_message(1, value);
        expect_dispatch(generic.result(), named.result(),
                        bytes({0x0a,
                               0x83, 0x80, 0x80, 0x80, 0x00,
                               0x08, 0x96, 0x01}),
                        "PB_MESSAGE encode dispatch");
    }
}


void test_all_pb_types_decode_independent_fixtures()
{
    expect_decode_dispatch<easypb::PB_INT32>(
        bytes({0x08,
               0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0x01}),
        int32_t(-1),
        [](easypb::Decoder& decoder) { return decoder.get_int32(); },
        "PB_INT32 decode dispatch");

    expect_decode_dispatch<easypb::PB_INT64>(
        bytes({0x08,
               0x80, 0x80, 0x80, 0x80, 0x80,
               0xe0, 0xff, 0xff, 0xff, 0x01}),
        int64_t(-1099511627776LL),
        [](easypb::Decoder& decoder) { return decoder.get_int64(); },
        "PB_INT64 decode dispatch");

    expect_decode_dispatch<easypb::PB_UINT32>(
        bytes({0x08, 0xe7, 0x8a, 0x8d, 0x89, 0x0f}),
        UINT32_C(0xf1234567),
        [](easypb::Decoder& decoder) { return decoder.get_uint32(); },
        "PB_UINT32 decode dispatch");

    expect_decode_dispatch<easypb::PB_UINT64>(
        bytes({0x08, 0x85, 0x80, 0x80, 0x80, 0x80, 0x20}),
        (UINT64_C(1) << 40) + 5,
        [](easypb::Decoder& decoder) { return decoder.get_uint64(); },
        "PB_UINT64 decode dispatch");

    expect_decode_dispatch<easypb::PB_SFIXED32>(
        bytes({0x0d, 0xfe, 0xff, 0xff, 0xff}),
        int32_t(-2),
        [](easypb::Decoder& decoder) { return decoder.get_sfixed32(); },
        "PB_SFIXED32 decode dispatch");

    expect_decode_dispatch<easypb::PB_SFIXED64>(
        bytes({0x09, 0xfd, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff}),
        int64_t(-3),
        [](easypb::Decoder& decoder) { return decoder.get_sfixed64(); },
        "PB_SFIXED64 decode dispatch");

    expect_decode_dispatch<easypb::PB_FIXED32>(
        bytes({0x0d, 0xef, 0xcd, 0xab, 0x89}),
        UINT32_C(0x89abcdef),
        [](easypb::Decoder& decoder) { return decoder.get_fixed32(); },
        "PB_FIXED32 decode dispatch");

    expect_decode_dispatch<easypb::PB_FIXED64>(
        bytes({0x09, 0xef, 0xcd, 0xab, 0x89,
               0x67, 0x45, 0x23, 0x01}),
        UINT64_C(0x0123456789abcdef),
        [](easypb::Decoder& decoder) { return decoder.get_fixed64(); },
        "PB_FIXED64 decode dispatch");

    expect_decode_dispatch<easypb::PB_SINT32>(
        bytes({0x08, 0x03}), int32_t(-2),
        [](easypb::Decoder& decoder) { return decoder.get_sint32(); },
        "PB_SINT32 decode dispatch");

    expect_decode_dispatch<easypb::PB_SINT64>(
        bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f}),
        int64_t(-1099511627776LL),
        [](easypb::Decoder& decoder) { return decoder.get_sint64(); },
        "PB_SINT64 decode dispatch");

    expect_decode_dispatch<easypb::PB_BOOL>(
        bytes({0x08, 0x02}), true,
        [](easypb::Decoder& decoder) { return decoder.get_bool(); },
        "PB_BOOL decode normalization");

    expect_decode_dispatch<easypb::PB_ENUM>(
        bytes({0x08, 0x96, 0x01}), int32_t(150),
        [](easypb::Decoder& decoder) { return decoder.get_enum(); },
        "PB_ENUM canonical decode dispatch");

    expect_decode_dispatch<easypb::PB_FLOAT>(
        bytes({0x0d, 0x00, 0x00, 0xc0, 0x3f}),
        1.5f,
        [](easypb::Decoder& decoder) { return decoder.get_float(); },
        "PB_FLOAT decode dispatch");

    expect_decode_dispatch<easypb::PB_DOUBLE>(
        bytes({0x09, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0xc0}),
        -2.0,
        [](easypb::Decoder& decoder) { return decoder.get_double(); },
        "PB_DOUBLE decode dispatch");

    {
        const std::string wire = bytes({0x0a, 0x03, 0x41, 0x00, 0x42});
        easypb::Decoder decoder(wire.data(), wire.size());
        std::string value;
        expect(decoder.get_next_field(), "PB_STRING decode field");
        decoder.get<easypb::PB_STRING>(&value);
        expect(value == bytes({0x41, 0x00, 0x42}), "PB_STRING decode dispatch");

        easypb::Decoder named_decoder(wire.data(), wire.size());
        std::string named_value;
        expect(named_decoder.get_next_field(), "named PB_STRING decode field");
        named_decoder.get_string(&named_value);
        expect(named_value == value, "named PB_STRING decode dispatch");
    }

    {
        const std::string wire = bytes({0x0a, 0x03, 0x00, 0xff, 0x7f});
        easypb::Decoder decoder(wire.data(), wire.size());
        std::string value;
        expect(decoder.get_next_field(), "PB_BYTES decode field");
        decoder.get<easypb::PB_BYTES>(&value);
        expect(value == bytes({0x00, 0xff, 0x7f}), "PB_BYTES decode dispatch");

        easypb::Decoder named_decoder(wire.data(), wire.size());
        std::string named_value;
        expect(named_decoder.get_next_field(), "named PB_BYTES decode field");
        named_decoder.get_bytes(&named_value);
        expect(named_value == value, "named PB_BYTES decode dispatch");
    }

    {
        const std::string wire = bytes({0x0a, 0x03, 0x08, 0x96, 0x01});
        easypb::Decoder decoder(wire.data(), wire.size());
        Child value;
        expect(decoder.get_next_field(), "PB_MESSAGE decode field");
        decoder.get<easypb::PB_MESSAGE>(&value);
        expect(value.value == 150, "PB_MESSAGE decode dispatch");

        easypb::Decoder named_decoder(wire.data(), wire.size());
        Child named_value;
        expect(named_decoder.get_next_field(), "named PB_MESSAGE decode field");
        named_decoder.get_message(&named_value);
        expect(named_value.value == value.value, "named PB_MESSAGE decode dispatch");
    }

    {
        const std::string wire = bytes({0x08, 0x96, 0x01});
        easypb::Decoder decoder(wire.data(), wire.size());
        DispatchEnum value = DispatchEnum::zero;
        expect(decoder.get_next_field(), "PB_ENUM scoped decode field");
        decoder.get<easypb::PB_ENUM>(&value);
        expect(value == DispatchEnum::value, "PB_ENUM scoped decode conversion");

        easypb::Decoder named_decoder(wire.data(), wire.size());
        DispatchEnum named_value = DispatchEnum::zero;
        expect(named_decoder.get_next_field(), "named PB_ENUM scoped decode field");
        named_decoder.get_enum(&named_value);
        expect(named_value == value, "named PB_ENUM scoped decode conversion");
    }
}


void test_unpacked_repeated_dispatch()
{
    const std::vector<int32_t> values{-1, 2};
    const std::string expected = bytes({0x08, 0x01, 0x08, 0x04});

    easypb::Encoder generic;
    easypb::Encoder named;
    generic.put_repeated<easypb::PB_SINT32>(1, values);
    named.put_repeated_sint32(1, values);
    expect_dispatch(generic.result(), named.result(), expected,
                    "unpacked repeated encode dispatch");

    std::vector<int32_t> generic_values;
    easypb::Decoder generic_decoder(expected.data(), expected.size());
    while (generic_decoder.get_next_field()) {
        generic_decoder.get_repeated<easypb::PB_SINT32>(&generic_values);
    }
    expect(generic_values == values, "unpacked repeated generic decode dispatch");

    std::vector<int32_t> named_values;
    easypb::Decoder named_decoder(expected.data(), expected.size());
    while (named_decoder.get_next_field()) {
        named_decoder.get_repeated_sint32(&named_values);
    }
    expect(named_values == values, "unpacked repeated named decode dispatch");
}


void test_packed_dispatch_and_canonical_narrowing()
{
    {
        const std::vector<int64_t> values{
            1,
            (INT64_C(1) << 32) + 2,
            -1,
        };
        const std::string expected = bytes({
            0x0a, 0x8c, 0x80, 0x80, 0x80, 0x00,
            0x01, 0x02,
            0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0x01,
        });
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_packed<easypb::PB_INT32>(1, values);
        named.put_packed_int32(1, values);
        expect_dispatch(generic.result(), named.result(), expected,
                        "packed PB_INT32 narrows each element");
    }

    {
        const std::vector<uint64_t> values{
            1,
            (UINT64_C(1) << 32) + 3,
        };
        const std::string expected = bytes({
            0x0a, 0x88, 0x80, 0x80, 0x80, 0x00,
            0x01, 0x00, 0x00, 0x00,
            0x03, 0x00, 0x00, 0x00,
        });
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_packed<easypb::PB_FIXED32>(1, values);
        named.put_packed_fixed32(1, values);
        expect_dispatch(generic.result(), named.result(), expected,
                        "packed PB_FIXED32 narrows each element");
    }

    {
        const std::vector<double> values{1.5, 3.25};
        const std::string expected = bytes({
            0x0a, 0x88, 0x80, 0x80, 0x80, 0x00,
            0x00, 0x00, 0xc0, 0x3f,
            0x00, 0x00, 0x50, 0x40,
        });
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_packed<easypb::PB_FLOAT>(1, values);
        named.put_packed_float(1, values);
        expect_dispatch(generic.result(), named.result(), expected,
                        "packed PB_FLOAT narrows each element");
    }

    {
        const std::vector<int> values{0, 2, -1};
        const std::string expected = bytes({
            0x0a, 0x83, 0x80, 0x80, 0x80, 0x00,
            0x00, 0x01, 0x01,
        });
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_packed<easypb::PB_BOOL>(1, values);
        named.put_packed_bool(1, values);
        expect_dispatch(generic.result(), named.result(), expected,
                        "packed PB_BOOL normalizes each element");
    }

    {
        const std::vector<DispatchEnum> values{
            DispatchEnum::zero,
            DispatchEnum::value,
        };
        const std::string expected = bytes({
            0x0a, 0x83, 0x80, 0x80, 0x80, 0x00,
            0x00, 0x96, 0x01,
        });
        easypb::Encoder generic;
        easypb::Encoder named;
        generic.put_packed<easypb::PB_ENUM>(1, values);
        named.put_packed_enum(1, values);
        expect_dispatch(generic.result(), named.result(), expected,
                        "packed PB_ENUM accepts scoped enums");
    }
}

}  // namespace


int main()
{
    test_all_pb_types_encode_byte_exact_and_match_named_api();
    test_all_pb_types_decode_independent_fixtures();
    test_unpacked_repeated_dispatch();
    test_packed_dispatch_and_canonical_narrowing();
    return failures == 0 ? 0 : 1;
}
