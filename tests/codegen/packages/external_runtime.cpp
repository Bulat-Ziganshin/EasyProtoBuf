#include <cstdint>
#include <string>

#include <easypb.hpp>

// W-68: `external.generated.hpp` includes its direct dependency, so the
// `other` declarations come from the generated `other.pb.hpp` instead of the
// hand-written block this file used to carry.
#include "external.generated.hpp"

int main()
{
    consumer::UseExternal value;
    value.state = other::State::READY;
    value.item.value = 42;
    value.implicit_state = other::State::READY;

    const consumer::UseExternal copy =
        easypb::decode<consumer::UseExternal>(easypb::encode(value));
    return copy.state == other::State::READY &&
           copy.item.value == 42 &&
           copy.implicit_state == other::State::READY ? 0 : 1;
}
