#include "package_runtime.hpp"

std::string encode_outer_in_helper(const foo::bar::Outer& value)
{
    return easypb::encode(value);
}

foo::bar::Outer decode_outer_in_helper(const std::string& wire)
{
    return easypb::decode<foo::bar::Outer>(wire);
}
