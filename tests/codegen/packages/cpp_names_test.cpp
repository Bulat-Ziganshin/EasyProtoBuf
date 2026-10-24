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

// CR-10: a name that is not a C++ identifier is refused, not escaped. The
// escape rule appends one underscore and cannot repair a leading digit.
bool rejects_identifier(const std::string& name)
{
    try {
        easypb_codegen::cpp_identifier(name);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

bool rejects_absolute_name(const std::string& name)
{
    try {
        easypb_codegen::cpp_absolute_name(name);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main()
{
    using easypb_codegen::cpp_absolute_name;
    using easypb_codegen::cpp_identifier;
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

    // CR-43: the same-package spelling escapes every component, so it names
    // exactly the declaration the absolute spelling names. The helper used
    // to strip the package and join raw components, which gave `class` for a
    // type the emitter declares as `class_`.
    CHECK(cpp_local_name(".pkg.class", "pkg") == "class_");
    CHECK(cpp_local_name(".pkg.Outer.class", "pkg") == "Outer::class_");
    CHECK(cpp_local_name(".pkg.class.Inner", "pkg") == "class_::Inner");
    CHECK(cpp_local_name(".Outer.class", "") == "Outer::class_");
    CHECK(cpp_absolute_name(".pkg.class") == "::pkg::class_");

    // A name that cannot be spelled verbatim in C++ is escaped; every other
    // name is returned unchanged, so a header without a reserved name keeps
    // its bytes.
    CHECK(cpp_identifier("class") == "class_");
    CHECK(cpp_identifier("template") == "template_");
    CHECK(cpp_identifier("namespace") == "namespace_");
    CHECK(cpp_identifier("NULL") == "NULL_");
    CHECK(cpp_identifier("assert") == "assert_");
    CHECK(cpp_identifier("value") == "value");
    CHECK(cpp_identifier("_value") == "_value");
    CHECK(cpp_identifier("value2") == "value2");
    // CR-10: no suffix can repair a name that is not an identifier at all,
    // so it is refused instead of emitted verbatim.
    CHECK(rejects_identifier(""));
    CHECK(rejects_identifier("0Zc"));
    CHECK(rejects_identifier("a-b"));
    CHECK(rejects_identifier("a b"));
    CHECK(rejects_identifier("a.b"));
    CHECK(rejects_absolute_name(".p.0X"));
    CHECK(rejects_absolute_name(".0p.M"));
    // The escaped name is the qualified name's component too.
    CHECK(cpp_absolute_name(".foo.class.Inner") ==
          "::foo::class_::Inner");
    CHECK(cpp_absolute_name(".foo.bar.Outer.Inner") ==
          "::foo::bar::Outer::Inner");

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
    // The two macro names the reference compiler escapes as well: a
    // namespace named after one of them does not compile either.
    CHECK(rejects_package("NULL"));
    CHECK(rejects_package("foo.assert"));

    return EXIT_SUCCESS;
}
