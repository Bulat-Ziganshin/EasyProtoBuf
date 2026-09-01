#include <easypb.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum State
{
    STATE_QUEUED = 0,
    STATE_RUNNING = 1,
    STATE_FINISHED = 2,
};

struct Attribute
{
    std::string name;
    double value = 0;
};

struct Job
{
    uint64_t id = 0;
    State state = STATE_QUEUED;
    std::vector<int32_t> deltas;
    std::map<std::string, uint32_t> counters;
    std::vector<Attribute> attributes;
};

// Encode every field to keep the introductory example independent of a
// particular default-value or presence policy.
inline void encode(easypb::Encoder& pb, const Attribute& value)
{
    pb.put_string(1, value.name);
    pb.put_double(2, value.value);
}

inline void encode(easypb::Encoder& pb, const Job& value)
{
    pb.put_uint64(1, value.id);
    pb.put_enum(2, value.state);
    pb.put_packed_sint32(3, value.deltas);
    pb.put_map_string_uint32(4, value.counters);
    pb.put_repeated_message(5, value.attributes);
}

inline void decode(easypb::Decoder pb, Attribute& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_string(&value.name); break;
            case 2: pb.get_double(&value.value); break;
            default: pb.skip_field();
        }
    }
}

inline void decode(easypb::Decoder pb, Job& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_uint64(&value.id); break;
            case 2: pb.get_enum(&value.state); break;
            case 3: pb.get_repeated_sint32(&value.deltas); break;
            case 4: pb.get_map_string_uint32(&value.counters); break;
            case 5: pb.get_repeated_message(&value.attributes); break;
            default: pb.skip_field();
        }
    }
}

// Compare all fields without relying on post-C++11 generated comparisons.
static bool equal(const Job& left, const Job& right)
{
    if (left.id != right.id ||
        left.state != right.state ||
        left.deltas != right.deltas ||
        left.counters != right.counters ||
        left.attributes.size() != right.attributes.size())
    {
        return false;
    }

    for (size_t index = 0; index < left.attributes.size(); ++index) {
        if (left.attributes[index].name != right.attributes[index].name ||
            left.attributes[index].value != right.attributes[index].value)
        {
            return false;
        }
    }
    return true;
}

int main()
{
    Job source;
    source.id = 42;
    source.state = STATE_RUNNING;
    source.deltas.push_back(-2);
    source.deltas.push_back(5);
    source.counters["warnings"] = 3;

    Attribute attribute;
    attribute.name = "cpu_load";
    attribute.value = 0.75;
    source.attributes.push_back(attribute);

    const std::string wire = easypb::encode(source);
    const Job copy = easypb::decode<Job>(wire);
    return equal(source, copy) ? 0 : 1;
}
