#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>

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

void expect_next_field(easypb::Decoder& decoder, uint32_t field_num,
                       easypb::WireType wire_type, const char* test_name)
{
    if (!decoder.get_next_field()) {
        std::cerr << test_name << ": no field found\n";
        ++failures;
        return;
    }

    if (decoder.field_num != field_num || decoder.wire_type != wire_type) {
        std::cerr << test_name << ": expected field " << field_num
                  << " with wire type " << wire_type << ", got field "
                  << decoder.field_num << " with wire type "
                  << decoder.wire_type << '\n';
        ++failures;
    }
}

template <typename ExpectedException>
void expect_skip_throws(const std::string& wire, const char* test_name)
{
    easypb::Decoder decoder(wire.data(), wire.size());
    if (!decoder.get_next_field()) {
        std::cerr << test_name << ": no field found\n";
        ++failures;
        return;
    }

    try {
        decoder.skip_field();
        std::cerr << test_name << ": expected an exception\n";
        ++failures;
    } catch (const ExpectedException&) {
        // Expected failure mode.
    } catch (const std::exception& error) {
        std::cerr << test_name << ": wrong exception: " << error.what() << '\n';
        ++failures;
    }
}

void test_skips_unknown_group_and_continues_with_following_field()
{
    const std::string wire = bytes({
        0x2b,                         // field 5: start group
        0x08, 0x96, 0x01,             // field 1: varint
        0x11, 0, 1, 2, 3, 4, 5, 6, 7, // field 2: fixed64
        0x1a, 0x03, 0xaa, 0xbb, 0xcc, // field 3: length-delimited
        0x25, 8, 9, 10, 11,           // field 4: fixed32
        0x33,                         // field 6: nested start group
        0x38, 0x01,                   // field 7: varint
        0x34,                         // field 6: nested end group
        0x2c,                         // field 5: end group
        0x08, 0x07                    // field 1: known field after the group
    });

    easypb::Decoder decoder(wire.data(), wire.size());
    expect_next_field(decoder, 5, easypb::WIRETYPE_START_GROUP,
                      "unknown group start");
    decoder.skip_field();

    expect_next_field(decoder, 1, easypb::WIRETYPE_VARINT,
                      "field after unknown group");
    const uint32_t value = decoder.get_uint32();
    if (value != 7) {
        std::cerr << "field after unknown group: expected 7, got "
                  << value << '\n';
        ++failures;
    }
    if (!decoder.eof()) {
        std::cerr << "field after unknown group: trailing bytes remain\n";
        ++failures;
    }
}

void test_rejects_mismatched_group_end()
{
    expect_skip_throws<easypb::wiretype_mismatch>(
        bytes({0x2b, 0x34}),
        "group end field number must match group start");

    expect_skip_throws<easypb::wiretype_mismatch>(
        bytes({0x2b, 0x33, 0x2c}),
        "nested group end field number must match innermost group");
}

void test_rejects_unterminated_group()
{
    expect_skip_throws<easypb::unexpected_eof>(
        bytes({0x2b, 0x08, 0x01}),
        "group must end before the input buffer");
}

void test_rejects_end_group_outside_a_group()
{
    expect_skip_throws<easypb::unsupported_wiretype>(
        bytes({0x2c}),
        "end group cannot be skipped as a standalone field");
}

void test_rejects_reserved_wire_types()
{
    expect_skip_throws<easypb::unsupported_wiretype>(
        bytes({0x2e}), "wire type 6 remains unsupported");
    expect_skip_throws<easypb::unsupported_wiretype>(
        bytes({0x2f}), "wire type 7 remains unsupported");
}

}  // namespace


int main()
{
    test_skips_unknown_group_and_continues_with_following_field();
    test_rejects_mismatched_group_end();
    test_rejects_unterminated_group();
    test_rejects_end_group_outside_a_group();
    test_rejects_reserved_wire_types();
    return failures == 0 ? 0 : 1;
}
