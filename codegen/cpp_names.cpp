#include "cpp_names.hpp"

#include <cctype>
#include <set>
#include <stdexcept>
#include <vector>

#include "schema.hpp"

namespace easypb_codegen {

// W-35: one guarded conversion door, declared in schema.hpp.
using easypb_schema::view_text;
namespace {

bool is_identifier_start(char c)
{
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalpha(value) != 0 || c == '_';
}

bool is_identifier_continue(char c)
{
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalnum(value) != 0 || c == '_';
}

// The names a generated C++ declaration may not carry verbatim.
//
// The list is the C++ keywords of the language the project builds for, plus
// the two macro names the reference compiler escapes as well: its own list
// (compiler/cpp/helpers.cc:120-217) holds "NULL" and "assert" among the
// keywords it resolves, and both are hazards in a generated header - "NULL"
// is a macro that expands to a literal, so a member named NULL would expand
// under the compiler.
//
// One set serves two rules, so the two rules cannot disagree: the package
// rule REFUSES a package component from this set (a namespace named after a
// keyword does not compile, and the project refuses it rather than escaping
// it), and the declaration rule ESCAPES a name from this set (a declaration
// named after a keyword does not compile either, and the reference compiler
// escapes it - see cpp_identifier).
const std::set<std::string>& cpp_reserved_names()
{
    static const char* const values[] = {
        "NULL", "alignas", "alignof", "and", "and_eq", "asm", "assert",
        "atomic_cancel", "atomic_commit", "atomic_noexcept", "auto", "bitand",
        "bitor", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "synchronized",
        "template", "this", "thread_local", "throw", "true", "try",
        "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };
    static const std::set<std::string> result(
        values, values + sizeof(values) / sizeof(values[0]));
    return result;
}

std::vector<std::string> split_components(const std::string& value,
                                          char separator,
                                          const char* what)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = value.find(separator, start);
        const std::string component = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (component.empty()) {
            throw std::runtime_error(std::string("invalid ") + what +
                                     ": empty name component");
        }
        result.push_back(component);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::string join_cpp(const std::vector<std::string>& components)
{
    std::string result;
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i != 0) result += "::";
        result += components[i];
    }
    return result;
}

// Every component of a dotted Protobuf name, in the one C++ spelling the
// emitter declares and references. Shared by both halves of cpp_local_name,
// so a name from the current package is escaped exactly like an absolute one
// (CR-43: the same-package branch used to return raw components and could
// hand back `class` where the emitter declares `class_`).
std::vector<std::string> cpp_name_components(const std::string& path)
{
    const std::vector<std::string> components =
        split_components(path, '.', "Protobuf type name");
    std::vector<std::string> result;
    result.reserve(components.size());
    for (std::size_t i = 0; i < components.size(); ++i) {
        result.push_back(cpp_identifier(components[i]));
    }
    return result;
}

// The [lex.name] rule `component` breaks, or null when it breaks none.
//
// [lex.name] reserves for the implementation every identifier that contains a
// double underscore and every identifier that begins with an underscore
// followed by an uppercase letter, in any scope; an identifier that begins
// with an underscore is additionally reserved in the global namespace. The
// return value is the clause of the rule, so the diagnostic can say why the
// name is refused instead of only that it is.
const char* reserved_identifier_form(const std::string& component,
                                     bool global_component)
{
    if (component.find("__") != std::string::npos) {
        return "it contains a double underscore";
    }
    if (component.size() >= 2 && component[0] == '_' &&
        std::isupper(static_cast<unsigned char>(component[1])) != 0) {
        return "it begins with an underscore followed by an uppercase letter";
    }
    if (global_component && !component.empty() && component[0] == '_') {
        return "it begins with an underscore in the global namespace";
    }
    return 0;
}

} // namespace

// The C++ name a Protobuf declaration name maps to.
//
// A name that cannot be spelled verbatim in C++ gets an underscore
// appended, exactly as the reference compiler does: its ResolveKeyword
// (compiler/cpp/helpers.cc:509-514) appends "_" to a keyword, and its
// ResolveKnownNameCollisions (helpers.cc:516-546) does the same for the
// names that would collide with a known method. Every other name is
// returned unchanged, so a header that carries no reserved name keeps the
// bytes it had.
//
// The suffix is the whole escape vocabulary, and it cannot repair a name
// that is not an identifier at all: `0Zc` stays a digit-first token
// whatever is appended. The reference compiler's ValidateSymbolName
// (descriptor.cc:5419-5438) has no first-character rule, so a descriptor set
// may carry such a name and protoc accepts it - and emits C++ that does not
// compile (`::int32_t 0zc_;`, measured against 30.1). This door refuses the
// name instead (CR-10): a generated header never carries a token C++ cannot
// read. The rule lives here, in the one function every declaration name goes
// through, so no caller can forget it.
std::string cpp_identifier(const std::string& name)
{
    if (name.empty() || !is_identifier_start(name[0])) {
        throw std::runtime_error(
            "Protobuf name '" + name + "' is not a valid C++ identifier");
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!is_identifier_continue(name[i])) {
            throw std::runtime_error(
                "Protobuf name '" + name + "' is not a valid C++ identifier");
        }
    }
    if (cpp_reserved_names().count(name) == 0) return name;
    return name + "_";
}

std::string cpp_absolute_name(const str_view& absolute_proto_name)
{
    if (absolute_proto_name.size() < 2 || absolute_proto_name[0] != '.') {
        throw std::runtime_error(
            "absolute Protobuf type name must begin with '.'");
    }

    // Every component of the path becomes a C++ name of its own: the package
    // components are namespaces and the rest are type names, and a name that
    // is a C++ keyword cannot be spelled verbatim in either place. The rule
    // is uniform because the reference compiler's is - it escapes a package
    // word (compiler/cpp/helpers.cc:548-557) the same way it escapes a type
    // name (helpers.cc:451-469). This project refuses a keyword package
    // component instead (validate_cpp_package), so in practice only the type
    // components are ever affected.
    const std::string path = view_text(absolute_proto_name);
    const std::vector<std::string> components =
        cpp_name_components(path.substr(1));

    std::string result;
    result.reserve(path.size() + 8);
    for (std::size_t i = 0; i < components.size(); ++i) {
        result += "::";
        result += components[i];
    }
    return result;
}

std::string cpp_local_name(const std::string& absolute_proto_name,
                           const std::string& package)
{
    if (absolute_proto_name.size() < 2 || absolute_proto_name[0] != '.') {
        throw std::runtime_error(
            "absolute Protobuf type name must begin with '.'");
    }

    if (package.empty()) {
        return join_cpp(cpp_name_components(absolute_proto_name.substr(1)));
    }

    const std::string prefix = "." + package + ".";
    if (absolute_proto_name.compare(0, prefix.size(), prefix) == 0) {
        return join_cpp(
            cpp_name_components(absolute_proto_name.substr(prefix.size())));
    }

    return cpp_absolute_name(absolute_proto_name);
}

std::string insertion_macro_stem(const std::string& absolute_proto_name,
                                 const std::string& package)
{
    if (absolute_proto_name.size() < 2 || absolute_proto_name[0] != '.') {
        throw std::runtime_error(
            "absolute Protobuf type name must begin with '.'");
    }

    std::size_t start = 1;
    if (!package.empty()) {
        if (absolute_proto_name.size() < package.size() + 2 ||
            absolute_proto_name.compare(1, package.size(), package) != 0 ||
            absolute_proto_name[package.size() + 1] != '.') {
            throw std::runtime_error(
                "insertion macro message name does not belong to the current package");
        }
        start = package.size() + 2;
    }

    if (start == absolute_proto_name.size() ||
        absolute_proto_name[start] == '.') {
        throw std::runtime_error(
            "invalid Protobuf message name: empty name component");
    }

    std::string result;
    result.reserve(7 + (absolute_proto_name.size() - start));
    result += "EASYPB_";
    for (std::size_t i = start; i < absolute_proto_name.size(); ++i) {
        const char c = absolute_proto_name[i];
        if (c == '.') {
            if (i + 1 == absolute_proto_name.size() ||
                absolute_proto_name[i + 1] == '.') {
                throw std::runtime_error(
                    "invalid Protobuf message name: empty name component");
            }
            result += '_';
        } else {
            result += c;
        }
    }
    return result;
}

// The global names the generated header's preamble makes visible.
//
// The list is grouped by the header that declares each group, and all of them
// reach every generated file: FILE_TEMPLATE includes <cstdint>, and it includes
// <easypb.hpp>, which includes <cstdint>, <cstddef> and <cstring> itself
// (include/easypb.hpp:11-14). The typedef names are the C names, not std::
// ones, because the emitter spells its scalar types absolutely (`::int32_t`),
// which is also why a generated `int32_t` cannot coexist with them. Only the C
// names are occupants: the `std::`-only spellings (`std::nullptr_t`,
// `std::byte`) are not in the global namespace and stay legal.
//
// The <cstring> names are the same kind of occupant as the typedefs:
// `void* memcpy(void*, const void*, size_t)` is declared in the global
// namespace next to `std::memcpy`, and `struct memcpy` or `namespace memcpy`
// is ill-formed next to it. The runtime calls `std::memcpy`
// (include/easypb.hpp:132, 340), so the function is part of the preamble's own
// set of names, not an accidental include.
//
// `__gnu_cxx` is the implementation namespace <cstdint> pulls in through
// <bits/c++config.h> on libstdc++; a global `struct __gnu_cxx` is ill-formed
// next to `namespace __gnu_cxx`. The name is also refused as a reserved form
// (`__`), but the two rules have to agree: a declaration that would collide
// with the library's namespace is an occupant of the file scope.
namespace {

struct GlobalNameGroup
{
    const char* kind;
    const char* const* names;
    std::size_t count;
};

// <cstdint>: exact-width, least-width, fast-width, pointer-width and
// maximum-width integer typedefs; <cstddef>: size, offset and alignment.
const char* const GLOBAL_TYPEDEF_NAMES[] = {
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int_least8_t", "int_least16_t", "int_least32_t", "int_least64_t",
    "uint_least8_t", "uint_least16_t", "uint_least32_t", "uint_least64_t",
    "int_fast8_t", "int_fast16_t", "int_fast32_t", "int_fast64_t",
    "uint_fast8_t", "uint_fast16_t", "uint_fast32_t", "uint_fast64_t",
    "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
    "size_t", "ptrdiff_t", "max_align_t"
};

// <cstring>: the C library names the header makes visible in the global
// namespace. The list is the whole <string.h> family, not only the one
// function the implementation calls, because every one of them is declared
// next to the generated declarations.
const char* const GLOBAL_C_FUNCTION_NAMES[] = {
    "memchr", "memcmp", "memcpy", "memmove", "memset",
    "strcat", "strchr", "strcmp", "strcoll", "strcpy", "strcspn",
    "strerror", "strlen", "strncat", "strncmp", "strncpy",
    "strpbrk", "strrchr", "strspn", "strstr", "strtok", "strxfrm"
};

// The implementation namespaces the standard library headers open in the
// global namespace.
const char* const GLOBAL_LIBRARY_NAMESPACE_NAMES[] = {
    "__gnu_cxx"
};

#define EASYPB_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

const GlobalNameGroup GLOBAL_NAME_GROUPS[] = {
    { "global typedef", GLOBAL_TYPEDEF_NAMES,
      EASYPB_COUNT_OF(GLOBAL_TYPEDEF_NAMES) },
    { "C library function", GLOBAL_C_FUNCTION_NAMES,
      EASYPB_COUNT_OF(GLOBAL_C_FUNCTION_NAMES) },
    { "C++ library namespace", GLOBAL_LIBRARY_NAMESPACE_NAMES,
      EASYPB_COUNT_OF(GLOBAL_LIBRARY_NAMESPACE_NAMES) }
};

const std::size_t GLOBAL_NAME_GROUP_COUNT =
    EASYPB_COUNT_OF(GLOBAL_NAME_GROUPS);

#undef EASYPB_COUNT_OF

std::set<std::string> build_global_names()
{
    std::set<std::string> result;
    for (std::size_t group = 0; group < GLOBAL_NAME_GROUP_COUNT; ++group) {
        const GlobalNameGroup& entry = GLOBAL_NAME_GROUPS[group];
        for (std::size_t i = 0; i < entry.count; ++i) {
            result.insert(entry.names[i]);
        }
    }
    return result;
}

} // namespace

const std::set<std::string>& cpp_global_names()
{
    static const std::set<std::string> result = build_global_names();
    return result;
}

const char* cpp_global_name_kind(const std::string& name)
{
    for (std::size_t group = 0; group < GLOBAL_NAME_GROUP_COUNT; ++group) {
        const GlobalNameGroup& entry = GLOBAL_NAME_GROUPS[group];
        for (std::size_t i = 0; i < entry.count; ++i) {
            if (name == entry.names[i]) return entry.kind;
        }
    }
    return 0;
}

void validate_cpp_declaration_name(const std::string& kind,
                                   const std::string& name,
                                   bool global_namespace)
{
    const char* rule = reserved_identifier_form(name, global_namespace);
    if (rule == 0) return;
    throw std::runtime_error(
        "The " + kind + " '" + name +
        "' uses a reserved C++ identifier form: " + rule);
}

void validate_cpp_package(const std::string& package)
{
    if (package.empty()) return;

    const std::vector<std::string> components =
        split_components(package, '.', "Protobuf package");
    for (std::size_t i = 0; i < components.size(); ++i) {
        const std::string& component = components[i];
        if (!is_identifier_start(component[0])) {
            throw std::runtime_error(
                "Protobuf package component '" + component +
                "' is not a valid C++ namespace identifier");
        }
        for (std::size_t j = 1; j < component.size(); ++j) {
            if (!is_identifier_continue(component[j])) {
                throw std::runtime_error(
                    "Protobuf package component '" + component +
                    "' is not a valid C++ namespace identifier");
            }
        }
        if (cpp_reserved_names().count(component) != 0) {
            throw std::runtime_error(
                "Protobuf package component '" + component +
                "' is a C++ keyword and cannot be used as a namespace");
        }
        const char* reserved_rule =
            reserved_identifier_form(component, i == 0);
        if (reserved_rule != 0) {
            throw std::runtime_error(
                "Protobuf package component '" + component +
                "' uses a reserved C++ identifier form: " + reserved_rule);
        }
        if (i == 0 && component == "std") {
            throw std::runtime_error(
                "Protobuf package cannot generate the global C++ namespace 'std'");
        }
        // The first component is the namespace the file's declarations open,
        // so it is spelled in the global namespace - where the preamble's own
        // typedefs and C library functions live. A later component is nested
        // and may shadow them without a redefinition
        // (`namespace foo { namespace int32_t { ... } }` is legal), so only
        // the first one is refused.
        if (i == 0 && cpp_global_names().count(component) != 0) {
            throw std::runtime_error(
                "Protobuf package component '" + component +
                "' is a global name the generated header's own preamble declares");
        }
    }
}

} // namespace easypb_codegen
