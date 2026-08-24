#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include <easypb.hpp>


namespace
{

int failures = 0;

// Build byte-exact wire fixtures without depending on the encoder under test.
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

void expect_bytes(const std::string& actual, const std::string& expected,
                  const char* test_name)
{
    if (actual != expected) {
        std::cerr << test_name << ": encoded bytes differ\n";
        ++failures;
    }
}

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected,
                  const char* test_name)
{
    if (actual != expected) {
        std::cerr << test_name << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

void expect_field(easypb::Decoder& decoder, uint32_t field_num,
                  const char* test_name)
{
    if (!decoder.get_next_field()) {
        std::cerr << test_name << ": no field found\n";
        ++failures;
        return;
    }
    expect_equal(decoder.field_num, field_num, test_name);
}

// Compare only the packed payload so the test accepts any valid varint length encoding.
void expect_packed_payload(const std::string& wire, const std::string& expected,
                           const char* test_name)
{
    easypb::Decoder decoder(wire.data(), wire.size());
    if (!decoder.get_next_field()) {
        std::cerr << test_name << ": no field found\n";
        ++failures;
        return;
    }
    if (decoder.field_num != 1 || decoder.wire_type != easypb::WIRETYPE_LENGTH_DELIMITED) {
        std::cerr << test_name << ": invalid packed field tag\n";
        ++failures;
        return;
    }

    const easypb::string_view payload = decoder.parse_bytearray_value();
    expect_bytes(std::string(payload.data(), payload.size()), expected, test_name);
    if (!decoder.eof()) {
        std::cerr << test_name << ": trailing bytes after packed field\n";
        ++failures;
    }
}

void test_packed_writer_converts_to_protobuf_type()
{
    {
        easypb::Encoder encoder;
        const std::vector<int64_t> values(1, -1);
        encoder.put_packed_uint32(1, values);
        expect_packed_payload(encoder.result(),
                              bytes({0xff, 0xff, 0xff, 0xff, 0x0f}),
                              "packed uint32 narrows before varint encoding");
    }

    {
        easypb::Encoder encoder;
        const std::vector<uint32_t> values(1, UINT32_MAX);
        encoder.put_packed_int32(1, values);
        expect_packed_payload(encoder.result(),
                              bytes({0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0x01}),
                              "packed int32 sign-extends after narrowing");
    }

    {
        easypb::Encoder encoder;
        const std::vector<int64_t> values(1, 1);
        encoder.put_packed_sfixed32(1, values);
        expect_packed_payload(encoder.result(), bytes({0x01, 0x00, 0x00, 0x00}),
                              "packed sfixed32 writes four bytes per converted value");
    }

    {
        easypb::Encoder encoder;
        const std::vector<uint32_t> values(1, 1);
        encoder.put_packed_fixed64(1, values);
        expect_packed_payload(encoder.result(),
                              bytes({0x01, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00}),
                              "packed fixed64 writes eight bytes per converted value");
    }

    {
        easypb::Encoder encoder;
        const std::vector<double> values(1, 1.0);
        encoder.put_packed_float(1, values);
        expect_packed_payload(encoder.result(), bytes({0x00, 0x00, 0x80, 0x3f}),
                              "packed float converts before fixed32 encoding");
    }

    {
        easypb::Encoder encoder;
        const std::vector<int> values(1, 2);
        encoder.put_packed_bool(1, values);
        expect_packed_payload(encoder.result(), bytes({0x01}),
                              "packed bool canonicalizes converted values");
    }

    {
        easypb::Encoder encoder;
        const std::vector<uint32_t> values(1, UINT32_MAX);
        encoder.put_packed_sint32(1, values);
        expect_packed_payload(encoder.result(), bytes({0x01}),
                              "packed sint32 narrows before ZigZag encoding");
    }
}

void test_scalar_reader_converts_through_protobuf_type()
{
    {
        const std::string wire = bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0x0f});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar int32 field");
        int64_t value = 0;
        decoder.get_int32(&value);
        expect_equal(value, int64_t(-1),
                     "scalar int32 narrows before destination assignment");
    }

    {
        const std::string wire = bytes({0x08,
                                        0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0x01});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar uint32 field");
        int64_t value = 0;
        decoder.get_uint32(&value);
        expect_equal(value, int64_t(UINT32_MAX),
                     "scalar uint32 narrows before destination assignment");
    }

    {
        const std::string wire = bytes({0x0d, 0xff, 0xff, 0xff, 0xff});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar sfixed32 field");
        int64_t value = 0;
        decoder.get_sfixed32(&value);
        expect_equal(value, int64_t(-1),
                     "scalar sfixed32 converts before destination assignment");
    }

    {
        const std::string wire = bytes({0x08, 0x02});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar bool field");
        int value = 0;
        decoder.get_bool(&value);
        expect_equal(value, 1, "scalar bool canonicalizes before destination assignment");
    }

    {
        // 16777217 is exactly representable as double but rounds to 16777216 as float.
        const std::string wire = bytes({0x09, 0x00, 0x00, 0x00, 0x10,
                                        0x00, 0x00, 0x70, 0x41});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar float field");
        double value = 0;
        decoder.get_float(&value);
        expect_equal(value, 16777216.0,
                     "scalar float rounds before destination assignment");
    }
}

void test_repeated_reader_converts_through_protobuf_type()
{
    {
        const std::string wire = bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0x0f});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "unpacked repeated int32 field");
        std::vector<int64_t> values;
        decoder.get_repeated_int32(&values);
        expect_equal(values.size(), size_t(1), "unpacked repeated int32 count");
        if (!values.empty()) {
            expect_equal(values[0], int64_t(-1),
                         "unpacked repeated int32 conversion");
        }
    }

    {
        const std::string wire = bytes({0x0a, 0x05,
                                        0xff, 0xff, 0xff, 0xff, 0x0f});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "packed repeated int32 field");
        std::vector<int64_t> values;
        decoder.get_repeated_int32(&values);
        expect_equal(values.size(), size_t(1), "packed repeated int32 count");
        if (!values.empty()) {
            expect_equal(values[0], int64_t(-1), "packed repeated int32 conversion");
        }
    }
}

void test_sint32_truncates_before_zigzag_decode()
{
    {
        const std::string wire = bytes({0x08, 0x80, 0x80, 0x80, 0x80, 0x10});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar sint32 field");
        expect_equal(decoder.get_sint32(), int32_t(0),
                     "scalar sint32 truncates raw varint before ZigZag decode");
    }

    {
        const std::string wire = bytes({0x0a, 0x05,
                                        0x80, 0x80, 0x80, 0x80, 0x10});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "packed repeated sint32 field");
        std::vector<int64_t> values;
        decoder.get_repeated_sint32(&values);
        expect_equal(values.size(), size_t(1), "packed repeated sint32 count");
        if (!values.empty()) {
            expect_equal(values[0], int64_t(0), "packed repeated sint32 conversion");
        }
    }

    {
        // The same raw varint remains 64-bit for a declared sint64 field.
        const std::string wire = bytes({0x08, 0x80, 0x80, 0x80, 0x80, 0x10});
        easypb::Decoder decoder(wire.data(), wire.size());
        expect_field(decoder, 1, "scalar sint64 field");
        expect_equal(decoder.get_sint64(), int64_t(2147483648LL),
                     "scalar sint64 retains 64-bit ZigZag semantics");
    }
}

}  // namespace


int main()
{
    test_packed_writer_converts_to_protobuf_type();
    test_scalar_reader_converts_through_protobuf_type();
    test_repeated_reader_converts_through_protobuf_type();
    test_sint32_truncates_before_zigzag_decode();
    return failures == 0 ? 0 : 1;
}
