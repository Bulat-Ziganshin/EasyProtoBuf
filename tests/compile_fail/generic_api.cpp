#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <easypb.hpp>


namespace compile_fail_fixture
{

enum class MapKeyEnum : int32_t
{
    zero = 0,
};

struct MapKeyMessage
{
    int32_t value = 0;
};


bool operator<(const MapKeyMessage& lhs, const MapKeyMessage& rhs)
{
    return lhs.value < rhs.value;
}


void encode(easypb::Encoder& pb, const MapKeyMessage& value)
{
    pb.put_int32(1, value.value);
}


void decode(easypb::Decoder pb, MapKeyMessage& value)
{
    while (pb.get_next_field()) {
        if (pb.field_num == 1) {
            pb.get_int32(&value.value);
        } else {
            pb.skip_field();
        }
    }
}

}  // namespace compile_fail_fixture


int main()
{
    using compile_fail_fixture::MapKeyEnum;
    using compile_fail_fixture::MapKeyMessage;

#if defined(EASYPB_CASE_MAP_FLOAT_PUT)
    std::map<float, int32_t> value;
    easypb::Encoder encoder;
    encoder.put_map<easypb::PB_FLOAT, easypb::PB_INT32>(1, value);
#elif defined(EASYPB_CASE_MAP_DOUBLE_PUT)
    std::map<double, int32_t> value;
    easypb::Encoder encoder;
    encoder.put_map<easypb::PB_DOUBLE, easypb::PB_INT32>(1, value);
#elif defined(EASYPB_CASE_MAP_BYTES_PUT)
    std::map<std::string, int32_t> value;
    easypb::Encoder encoder;
    encoder.put_map<easypb::PB_BYTES, easypb::PB_INT32>(1, value);
#elif defined(EASYPB_CASE_MAP_ENUM_PUT)
    std::map<MapKeyEnum, int32_t> value;
    easypb::Encoder encoder;
    encoder.put_map<easypb::PB_ENUM, easypb::PB_INT32>(1, value);
#elif defined(EASYPB_CASE_MAP_MESSAGE_PUT)
    std::map<MapKeyMessage, int32_t> value;
    easypb::Encoder encoder;
    encoder.put_map<easypb::PB_MESSAGE, easypb::PB_INT32>(1, value);
#elif defined(EASYPB_CASE_MAP_FLOAT_GET)
    const char wire[] = "";
    std::map<float, int32_t> value;
    easypb::Decoder decoder(wire, 0);
    decoder.get_map<easypb::PB_FLOAT, easypb::PB_INT32>(&value);
#elif defined(EASYPB_CASE_MAP_DOUBLE_GET)
    const char wire[] = "";
    std::map<double, int32_t> value;
    easypb::Decoder decoder(wire, 0);
    decoder.get_map<easypb::PB_DOUBLE, easypb::PB_INT32>(&value);
#elif defined(EASYPB_CASE_MAP_BYTES_GET)
    const char wire[] = "";
    std::map<std::string, int32_t> value;
    easypb::Decoder decoder(wire, 0);
    decoder.get_map<easypb::PB_BYTES, easypb::PB_INT32>(&value);
#elif defined(EASYPB_CASE_MAP_ENUM_GET)
    const char wire[] = "";
    std::map<MapKeyEnum, int32_t> value;
    easypb::Decoder decoder(wire, 0);
    decoder.get_map<easypb::PB_ENUM, easypb::PB_INT32>(&value);
#elif defined(EASYPB_CASE_MAP_MESSAGE_GET)
    const char wire[] = "";
    std::map<MapKeyMessage, int32_t> value;
    easypb::Decoder decoder(wire, 0);
    decoder.get_map<easypb::PB_MESSAGE, easypb::PB_INT32>(&value);
#elif defined(EASYPB_CASE_PACKED_STRING)
    const std::vector<std::string> value(1, "value");
    easypb::Encoder encoder;
    encoder.put_packed<easypb::PB_STRING>(1, value);
#elif defined(EASYPB_CASE_PACKED_BYTES)
    const std::vector<std::string> value(1, "value");
    easypb::Encoder encoder;
    encoder.put_packed<easypb::PB_BYTES>(1, value);
#elif defined(EASYPB_CASE_PACKED_MESSAGE)
    const std::vector<MapKeyMessage> value(1);
    easypb::Encoder encoder;
    encoder.put_packed<easypb::PB_MESSAGE>(1, value);
#elif defined(EASYPB_CASE_GET_MESSAGE_VALUE)
    const char wire[] = "";
    easypb::Decoder decoder(wire, 0);
    decoder.get<easypb::PB_MESSAGE>();
#else
#error "A compile-fail case must be selected"
#endif

    return 0;
}
