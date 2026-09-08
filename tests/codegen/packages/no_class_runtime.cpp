#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <easypb.hpp>

namespace foo {
namespace bar {

enum State : ::int32_t
{
    READY = 7
};

struct Outer
{
    struct Inner
    {
        ::int32_t value = 0;
        bool has_value = false;
    };

    Inner child;
    State state = READY;
    bool has_child = false;
    bool has_state = false;
};

} // namespace bar
} // namespace foo

#include "names.no-class.generated.hpp"

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr "\n"; \
        return EXIT_FAILURE; \
    } \
} while (false)

int main()
{
    foo::bar::Outer source;
    source.child.value = 42;
    source.state = foo::bar::State::READY;

    const std::string wire = easypb::encode(source);
    const foo::bar::Outer copy = easypb::decode<foo::bar::Outer>(wire);
    CHECK(copy.child.value == 42);
    CHECK(copy.state == foo::bar::State::READY);
    return EXIT_SUCCESS;
}
