#include <cstdint>
#include <string>

#include <easypb.hpp>

namespace other {

enum State : ::int32_t { READY = 7 };

struct External
{
    ::int32_t value = 0;
    bool has_value = false;
};

inline void encode(::easypb::Encoder& pb, const External& value)
{
    pb.put_int32(1, value.value);
}

inline void decode(::easypb::Decoder pb, External& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_int32(&value.value, &value.has_value); break;
            default: pb.skip_field();
        }
    }
}

} // namespace other

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
