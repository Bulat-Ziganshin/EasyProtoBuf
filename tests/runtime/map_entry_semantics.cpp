#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <easypb.hpp>


namespace
{

int failures = 0;

struct Value
{
    int32_t a = 0;
    int32_t b = 0;
};

struct Holder
{
    Value singular;
    std::vector<Value> repeated;
    std::map<std::string, Value> mapped;
};


// Generated decoders merge into their supplied object rather than clearing it.
void decode(easypb::Decoder pb, Value& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_int32(&value.a); break;
            case 2: pb.get_int32(&value.b); break;
            default: pb.skip_field();
        }
    }
}


void decode(easypb::Decoder pb, Holder& holder)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_message(&holder.singular); break;
            case 2: pb.get_repeated_message(&holder.repeated); break;
            case 3: pb.get_map_string_message(&holder.mapped); break;
            default: pb.skip_field();
        }
    }
}


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


void expect(bool condition, const char* test_name)
{
    if (!condition) {
        std::cerr << test_name << '\n';
        ++failures;
    }
}


std::map<std::string, int32_t> decode_scalar_map(const std::string& wire)
{
    easypb::Decoder decoder(wire.data(), wire.size());
    std::map<std::string, int32_t> result;
    while (decoder.get_next_field()) {
        if (decoder.field_num == 1) {
            decoder.get_map_string_int32(&result);
        } else {
            decoder.skip_field();
        }
    }
    return result;
}


void expect_single_scalar_entry(const std::string& wire,
                                const std::string& expected_key,
                                int32_t expected_value,
                                const char* test_name)
{
    const std::map<std::string, int32_t> result = decode_scalar_map(wire);
    const std::map<std::string, int32_t>::const_iterator entry =
        result.find(expected_key);
    expect(result.size() == 1 && entry != result.end() &&
               entry->second == expected_value,
           test_name);
}


void test_map_entry_defaults_and_unknown_fields()
{
    expect_single_scalar_entry(
        bytes({0x0a, 0x00}), "", 0,
        "empty map entry must insert the default key and value");

    expect_single_scalar_entry(
        bytes({0x0a, 0x03, 0x0a, 0x01, 0x61}), "a", 0,
        "map entry without field 2 must use the default value");

    expect_single_scalar_entry(
        bytes({0x0a, 0x02, 0x10, 0x07}), "", 7,
        "map entry without field 1 must use the default key");

    expect_single_scalar_entry(
        bytes({0x0a, 0x02, 0x18, 0x63}), "", 0,
        "unknown-only map entry must still insert both defaults");

    expect_single_scalar_entry(
        bytes({0x0a, 0x07,
               0x0a, 0x01, 0x75,
               0x18, 0x63,
               0x10, 0x09}),
        "u", 9,
        "unknown map-entry fields must be skipped without dropping the entry");

    expect_single_scalar_entry(
        bytes({0x0a, 0x05,
               0x10, 0x05,
               0x0a, 0x01, 0x6f}),
        "o", 5,
        "map key and value fields may appear in either order");
}


void test_map_entry_duplicate_fields()
{
    expect_single_scalar_entry(
        bytes({0x0a, 0x0a,
               0x0a, 0x01, 0x61,
               0x0a, 0x01, 0x62,
               0x10, 0x01,
               0x10, 0x02}),
        "b", 2,
        "last scalar key and value inside one map entry must win");

    expect_single_scalar_entry(
        bytes({0x0a, 0x05, 0x0a, 0x01, 0x78, 0x10, 0x01,
               0x0a, 0x05, 0x0a, 0x01, 0x78, 0x10, 0x02}),
        "x", 2,
        "last outer map entry with a duplicate key must win");
}


void test_scalar_field_cardinalities()
{
    const std::string wire = bytes({
        0x08, 0x01,
        0x08, 0x02,
        0x10, 0x03,
        0x10, 0x04,
        0x12, 0x02, 0x05, 0x06,
    });

    easypb::Decoder decoder(wire.data(), wire.size());
    int32_t singular = 0;
    std::vector<int32_t> repeated;
    while (decoder.get_next_field()) {
        switch (decoder.field_num) {
            case 1: decoder.get_int32(&singular); break;
            case 2: decoder.get_repeated_int32(&repeated); break;
            default: decoder.skip_field();
        }
    }

    expect(singular == 2,
           "last occurrence of a singular scalar field must win");
    expect(repeated.size() == 4 &&
               repeated[0] == 3 && repeated[1] == 4 &&
               repeated[2] == 5 && repeated[3] == 6,
           "unpacked and packed repeated scalar occurrences must append");
}


void test_message_field_cardinalities()
{
    const std::string wire = bytes({
        // Singular message field: two occurrences merge into one object.
        0x0a, 0x02, 0x08, 0x0a,
        0x0a, 0x02, 0x10, 0x14,

        // Repeated message field: two occurrences create two elements.
        0x12, 0x02, 0x08, 0x01,
        0x12, 0x02, 0x10, 0x02,

        // One map entry with two message-valued field occurrences: merge.
        0x1a, 0x10,
        0x0a, 0x06, 0x6d, 0x65, 0x72, 0x67, 0x65, 0x64,
        0x12, 0x02, 0x08, 0x0a,
        0x12, 0x02, 0x10, 0x14,

        // Two entries with the same key: the latter replaces the former.
        0x1a, 0x0d,
        0x0a, 0x07, 0x72, 0x65, 0x70, 0x6c, 0x61, 0x63, 0x65,
        0x12, 0x02, 0x08, 0x0a,
        0x1a, 0x0d,
        0x0a, 0x07, 0x72, 0x65, 0x70, 0x6c, 0x61, 0x63, 0x65,
        0x12, 0x02, 0x10, 0x14,

        // A missing message value creates its default instance.
        0x1a, 0x07,
        0x0a, 0x05, 0x65, 0x6d, 0x70, 0x74, 0x79,

        // A missing key uses the default empty string.
        0x1a, 0x04,
        0x12, 0x02, 0x08, 0x03,
    });

    const Holder holder = easypb::decode<Holder>(wire);
    expect(holder.singular.a == 10 && holder.singular.b == 20,
           "singular message occurrences must merge");
    expect(holder.repeated.size() == 2 &&
               holder.repeated[0].a == 1 && holder.repeated[0].b == 0 &&
               holder.repeated[1].a == 0 && holder.repeated[1].b == 2,
           "repeated message occurrences must create distinct elements");
    expect(holder.mapped.size() == 4,
           "each distinct map key must create one map element");

    const std::map<std::string, Value>::const_iterator merged =
        holder.mapped.find("merged");
    expect(merged != holder.mapped.end() &&
               merged->second.a == 10 && merged->second.b == 20,
           "message occurrences inside one map entry must merge");

    const std::map<std::string, Value>::const_iterator replaced =
        holder.mapped.find("replace");
    expect(replaced != holder.mapped.end() &&
               replaced->second.a == 0 && replaced->second.b == 20,
           "duplicate map entries must replace rather than merge values");

    const std::map<std::string, Value>::const_iterator empty =
        holder.mapped.find("empty");
    expect(empty != holder.mapped.end() &&
               empty->second.a == 0 && empty->second.b == 0,
           "missing map message value must use its default instance");

    const std::map<std::string, Value>::const_iterator default_key =
        holder.mapped.find("");
    expect(default_key != holder.mapped.end() &&
               default_key->second.a == 3 && default_key->second.b == 0,
           "missing map key must use the key type default");
}

}  // namespace


int main()
{
    test_map_entry_defaults_and_unknown_fields();
    test_map_entry_duplicate_fields();
    test_scalar_field_cardinalities();
    test_message_field_cardinalities();
    return failures == 0 ? 0 : 1;
}
