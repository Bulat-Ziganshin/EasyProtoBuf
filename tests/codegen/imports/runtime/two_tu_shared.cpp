// Plan 5's two-TU runtime proof, second translation unit: it includes the
// shared dependency's header directly and uses its codecs, while the first
// translation unit reaches the same dependency through the generated root
// header. The two definitions below are the cross-TU contract the main file
// calls.

#include <string>

#include "common/node.pb.hpp"

common::Node make_shared_node(::int32_t value)
{
    common::Node node;
    node.data.value = value;
    return node;
}

bool shared_node_round_trip(::int32_t value)
{
    const common::Node source = make_shared_node(value);
    const std::string data = easypb::encode(source);
    const common::Node copy = easypb::decode<common::Node>(data);
    return copy.data.value == value;
}
