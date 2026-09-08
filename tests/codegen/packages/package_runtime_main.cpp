#include <cstdlib>
#include <iostream>

#include "package_runtime.hpp"

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
    source.injected = 9;

    const std::string wire = encode_outer_in_helper(source);
    const foo::bar::Outer copy = decode_outer_in_helper(wire);
    CHECK(copy.child.value == 42);
    CHECK(copy.state == foo::bar::State::READY);
    CHECK(copy.injected == 9);
    CHECK(copy.post_decoded);

    alpha::Box a;
    beta::Box b;
    a.value = 17;
    b.text = "same C++ leaf name, different package";
    CHECK(easypb::decode<alpha::Box>(easypb::encode(a)).value == 17);
    CHECK(easypb::decode<beta::Box>(easypb::encode(b)).text == b.text);

    app::std::easypb::Shadow shadow;
    shadow.text = "qualified runtime names";
    shadow.value = 23;
    shadow.value64 = -1234567890123LL;
    shadow.uvalue = 4000000000U;
    shadow.uvalue64 = 9000000000000ULL;
    shadow.values.push_back(-7);
    shadow.values.push_back(11);
    shadow.lookup["answer"] = 42;
    shadow.kind = app::std::easypb::Kind::ONE;
    const app::std::easypb::Shadow shadow_copy =
        easypb::decode<app::std::easypb::Shadow>(easypb::encode(shadow));
    CHECK(shadow_copy.text == shadow.text);
    CHECK(shadow_copy.value == shadow.value);
    CHECK(shadow_copy.value64 == shadow.value64);
    CHECK(shadow_copy.uvalue == shadow.uvalue);
    CHECK(shadow_copy.uvalue64 == shadow.uvalue64);
    CHECK(shadow_copy.values == shadow.values);
    CHECK(shadow_copy.lookup == shadow.lookup);
    CHECK(shadow_copy.kind == shadow.kind);

    app::module::Request module_request;
    module_request.value = 31;
    CHECK(easypb::decode<app::module::Request>(easypb::encode(module_request)).value == 31);

    app::import::Request import_request;
    import_request.text = "contextual namespace identifier";
    CHECK(easypb::decode<app::import::Request>(easypb::encode(import_request)).text == import_request.text);

    return EXIT_SUCCESS;
}
