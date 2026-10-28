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

// V-02: a name whose spelling [lex.name] reserves is refused as a
// declaration, not escaped.
bool rejects_declaration(const std::string& name, bool global_namespace)
{
    try {
        easypb_codegen::validate_cpp_declaration_name("message", name,
                                                      global_namespace);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main()
{
    using easypb_codegen::cpp_absolute_name;
    using easypb_codegen::cpp_global_name_kind;
    using easypb_codegen::cpp_global_names;
    using easypb_codegen::cpp_identifier;
    using easypb_codegen::cpp_local_name;
    using easypb_codegen::insertion_macro_stem;
    using easypb_codegen::validate_cpp_declaration_name;
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
    // U-02 (owner decision #1, variant A): `package easypb;` is allowed. The
    // runtime names are occupants of that file scope, not a reason to refuse
    // the namespace itself.
    validate_cpp_package("easypb");
    validate_cpp_package("easypb.sub");
    // U-03: only the FIRST component is spelled in the global namespace, where
    // the preamble's typedefs live. A later component is nested and may shadow
    // them without a redefinition (`tests/codegen/packages/data/shadow.proto`
    // declares `message int32_t` in `package app.std.easypb`).
    validate_cpp_package("foo.int32_t");
    validate_cpp_package("foo.size_t.bar");

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
    // U-03: the first package component is a namespace in the global
    // namespace, next to the typedefs the generated preamble declares.
    CHECK(rejects_package("int32_t"));
    CHECK(rejects_package("uint64_t"));
    CHECK(rejects_package("size_t"));
    CHECK(rejects_package("max_align_t"));
    CHECK(rejects_package("intptr_t.app"));

    // U-03: the list is the family the preamble provides, not the five names a
    // bug report happened to try: both <cstdint> groups and the <cstddef>
    // typedefs are occupants of the global namespace.
    CHECK(cpp_global_names().count("int8_t") == 1);
    CHECK(cpp_global_names().count("int32_t") == 1);
    CHECK(cpp_global_names().count("uint64_t") == 1);
    CHECK(cpp_global_names().count("int_least16_t") == 1);
    CHECK(cpp_global_names().count("uint_fast64_t") == 1);
    CHECK(cpp_global_names().count("intmax_t") == 1);
    CHECK(cpp_global_names().count("size_t") == 1);
    CHECK(cpp_global_names().count("ptrdiff_t") == 1);
    CHECK(cpp_global_names().count("max_align_t") == 1);
    // A name the preamble does not provide, and a runtime type (which lives in
    // namespace easypb, not in the global namespace), stay out of the list.
    CHECK(cpp_global_names().count("wxWidget") == 0);
    CHECK(cpp_global_names().count("Encoder") == 0);
    CHECK(cpp_global_names().count("std") == 0);

    // V-02: the preamble also puts the <cstring> functions and the
    // implementation namespace <cstdint> pulls in into the global namespace,
    // so they are occupants of an un-packaged file scope and refused package
    // components as well.
    CHECK(cpp_global_names().count("memcpy") == 1);
    CHECK(cpp_global_names().count("memset") == 1);
    CHECK(cpp_global_names().count("strlen") == 1);
    CHECK(cpp_global_names().count("__gnu_cxx") == 1);
    CHECK(std::string(cpp_global_name_kind("int32_t")) == "global typedef");
    CHECK(std::string(cpp_global_name_kind("memcpy")) ==
          "C library function");
    CHECK(std::string(cpp_global_name_kind("__gnu_cxx")) ==
          "C++ library namespace");
    CHECK(cpp_global_name_kind("wxWidget") == 0);
    CHECK(rejects_package("memcpy"));
    CHECK(rejects_package("strlen"));

    // V-02: declarations. [lex.name] reserves `__` and `_Upper` in every
    // scope and a leading `_` in the global namespace; a keyword is escaped,
    // not refused, and a case difference or an already escaped name is not a
    // reserved form.
    CHECK(rejects_declaration("_Foo", false));
    CHECK(rejects_declaration("_Foo", true));
    CHECK(rejects_declaration("foo__bar", false));
    CHECK(rejects_declaration("foo__bar", true));
    CHECK(rejects_declaration("_foo", true));
    CHECK(!rejects_declaration("_foo", false));
    CHECK(!rejects_declaration("foo_bar", false));
    CHECK(!rejects_declaration("class", false));
    CHECK(!rejects_declaration("class_", false));
    CHECK(!rejects_declaration("Foo", false));
    CHECK(!rejects_declaration("encoder", false));

    return EXIT_SUCCESS;
}
