#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "cpp_names.hpp"

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr "\n"; \
        return EXIT_FAILURE; \
    } \
} while (false)

namespace {

bool rejects_package(const std::string& package)
{
    try {
        easypb_codegen::validate_cpp_package(package);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main()
{
    using easypb_codegen::cpp_absolute_name;
    using easypb_codegen::cpp_local_name;
    using easypb_codegen::insertion_macro_stem;
    using easypb_codegen::validate_cpp_package;

    CHECK(cpp_absolute_name(".foo.bar.Outer.Inner") ==
          "::foo::bar::Outer::Inner");
    CHECK(cpp_local_name(".foo.bar.Outer.Inner", "foo.bar") ==
          "Outer::Inner");
    CHECK(cpp_local_name(".foo.barn.Thing", "foo.bar") ==
          "::foo::barn::Thing");
    CHECK(cpp_local_name(".other.Thing", "foo.bar") ==
          "::other::Thing");
    CHECK(cpp_local_name(".Outer.Inner", "") == "Outer::Inner");

    CHECK(insertion_macro_stem(".foo.bar.Outer", "foo.bar") ==
          "EASYPB_Outer");
    CHECK(insertion_macro_stem(".foo.bar.Outer.Inner", "foo.bar") ==
          "EASYPB_Outer_Inner");
    CHECK(insertion_macro_stem(".Outer.Inner", "") ==
          "EASYPB_Outer_Inner");
    // Preserve the historical non-reversible underscore encoding.
    CHECK(insertion_macro_stem(".foo.Outer_Inner", "foo") ==
          insertion_macro_stem(".foo.Outer.Inner", "foo"));

    validate_cpp_package("");
    validate_cpp_package("foo");
    validate_cpp_package("foo.bar");
    validate_cpp_package("foo.std.easypb");
    validate_cpp_package("foo_bar.baz2");
    validate_cpp_package("app.module");
    validate_cpp_package("app.import");

    CHECK(rejects_package("std"));
    CHECK(rejects_package("std.foo"));
    CHECK(rejects_package("foo.class"));
    CHECK(rejects_package("foo.__detail"));
    CHECK(rejects_package("_global.foo"));
    CHECK(rejects_package("foo.bad-name"));

    return EXIT_SUCCESS;
}
