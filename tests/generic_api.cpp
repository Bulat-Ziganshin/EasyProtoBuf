#include <easypb.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct GenericChild
{
    int32_t value = 0;
};


enum class GenericEnum : int32_t
{
    zero = 0,
    seven = 7,
};


struct UserId
{
    explicit UserId(int32_t x) : value(x) {}
    explicit operator int32_t() const { return value; }

    int32_t value;
};

inline void encode(easypb::Encoder& pb, const GenericChild& x)
{
    pb.put<easypb::PB_INT32>(1, x.value);
}

inline void decode(easypb::Decoder pb, GenericChild& x)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get<easypb::PB_INT32>(&x.value); break;
            default: pb.skip_field();
        }
    }
}

int main()
{
    const std::vector<int32_t> repeated{1, -2, 3};
    const std::vector<int32_t> packed{4, 5, 6};
    const std::map<int32_t, std::string> mapped{{7, "seven"}, {8, "eight"}};
    GenericChild child;
    child.value = 9;

    easypb::Encoder generic;
    generic.put<easypb::PB_INT32>(1, 123);
    generic.put_repeated<easypb::PB_SINT32>(2, repeated);
    generic.put_map<easypb::PB_INT32, easypb::PB_STRING>(3, mapped);
    generic.put<easypb::PB_MESSAGE>(4, child);
    generic.put_packed<easypb::PB_INT32>(5, packed);
    const std::string generic_data = generic.result();

    easypb::Encoder named;
    named.put_int32(1, 123);
    named.put_repeated_sint32(2, repeated);
    named.put_map_int32_string(3, mapped);
    named.put_message(4, child);
    named.put_packed_int32(5, packed);
    if (generic_data != named.result())  return 1;

    int32_t scalar = 0;
    std::vector<int32_t> decoded_repeated;
    std::map<int32_t, std::string> decoded_map;
    GenericChild decoded_child;
    std::vector<int32_t> decoded_packed;

    easypb::Decoder decoder(generic_data.data(), generic_data.size());
    while (decoder.get_next_field()) {
        switch (decoder.field_num) {
            case 1: decoder.get<easypb::PB_INT32>(&scalar); break;
            case 2: decoder.get_repeated<easypb::PB_SINT32>(&decoded_repeated); break;
            case 3: decoder.get_map<easypb::PB_INT32, easypb::PB_STRING>(&decoded_map); break;
            case 4: decoder.get<easypb::PB_MESSAGE>(&decoded_child); break;
            case 5: decoder.get_repeated<easypb::PB_INT32>(&decoded_packed); break;
            default: decoder.skip_field();
        }
    }

    if (scalar != 123) return 2;
    if (decoded_repeated != repeated) return 3;
    if (decoded_map != mapped) return 4;
    if (decoded_child.value != child.value) return 5;
    if (decoded_packed != packed) return 6;

    // Packed encoding must use the protobuf field type, not the C++ container
    // element width. These values intentionally require narrowing first.
    const std::vector<int64_t> packed_int32_wide{1, (int64_t(1) << 32) + 2, -1};
    const std::vector<uint64_t> packed_fixed32_wide{1, (uint64_t(1) << 32) + 3};
    const std::vector<double> packed_float_wide{1.5, 3.25};

    easypb::Encoder packed_encoder;
    packed_encoder.put_packed<easypb::PB_INT32>(1, packed_int32_wide);
    packed_encoder.put_packed<easypb::PB_FIXED32>(2, packed_fixed32_wide);
    packed_encoder.put_packed<easypb::PB_FLOAT>(3, packed_float_wide);
    const std::string packed_data = packed_encoder.result();

    std::vector<int32_t> narrowed_int32;
    std::vector<uint32_t> narrowed_fixed32;
    std::vector<float> narrowed_float;
    easypb::Decoder packed_decoder(packed_data.data(), packed_data.size());
    while (packed_decoder.get_next_field()) {
        switch (packed_decoder.field_num) {
            case 1: packed_decoder.get_repeated<easypb::PB_INT32>(&narrowed_int32); break;
            case 2: packed_decoder.get_repeated<easypb::PB_FIXED32>(&narrowed_fixed32); break;
            case 3: packed_decoder.get_repeated<easypb::PB_FLOAT>(&narrowed_float); break;
            default: packed_decoder.skip_field();
        }
    }

    const std::vector<int32_t> expected_int32{1, 2, -1};
    const std::vector<uint32_t> expected_fixed32{1, 3};
    const std::vector<float> expected_float{1.5f, 3.25f};
    if (narrowed_int32 != expected_int32) return 7;
    if (narrowed_fixed32 != expected_fixed32) return 8;
    if (narrowed_float != expected_float) return 9;

    // Every generic and named scalar entry point must preserve explicit
    // conversion through the canonical protobuf C++ type.
    const std::vector<GenericEnum> enum_values{
        GenericEnum::zero,
        GenericEnum::seven,
    };
    const UserId user_id(42);
    easypb::Encoder enum_encoder;
    enum_encoder.put<easypb::PB_ENUM>(1, GenericEnum::seven);
    enum_encoder.put_packed_enum(2, enum_values);
    enum_encoder.put_enum(3, GenericEnum::seven);
    enum_encoder.put_int32(4, user_id);
    const std::string enum_data = enum_encoder.result();

    GenericEnum decoded_enum = GenericEnum::zero;
    std::vector<GenericEnum> decoded_enum_values;
    GenericEnum decoded_named_enum = GenericEnum::zero;
    UserId decoded_user_id(0);
    easypb::Decoder enum_decoder(enum_data.data(), enum_data.size());
    while (enum_decoder.get_next_field()) {
        switch (enum_decoder.field_num) {
            case 1: enum_decoder.get<easypb::PB_ENUM>(&decoded_enum); break;
            case 2: enum_decoder.get_repeated<easypb::PB_ENUM>(&decoded_enum_values); break;
            case 3: enum_decoder.get_enum(&decoded_named_enum); break;
            case 4: enum_decoder.get_int32(&decoded_user_id); break;
            default: enum_decoder.skip_field();
        }
    }

    if (decoded_enum != GenericEnum::seven) return 10;
    if (decoded_enum_values != enum_values) return 11;
    if (decoded_named_enum != GenericEnum::seven) return 12;
    if (decoded_user_id.value != user_id.value) return 13;
    return 0;
}
