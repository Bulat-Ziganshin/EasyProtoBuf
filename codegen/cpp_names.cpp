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

bool has_reserved_identifier_form(const std::string& component,
                                  bool global_component)
{
    if (component.find("__") != std::string::npos) return true;
    if (component.size() >= 2 && component[0] == '_' &&
        std::isupper(static_cast<unsigned char>(component[1])) != 0) {
        return true;
    }
    // Names beginning with an underscore are reserved in the global namespace.
    if (global_component && !component.empty() && component[0] == '_') {
        return true;
    }
    return false;
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
std::string cpp_identifier(const std::string& name)
{
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
    const std::vector<std::string> components = split_components(
        path.substr(1), '.', "Protobuf type name");

    std::string result;
    result.reserve(path.size() + 8);
    for (std::size_t i = 0; i < components.size(); ++i) {
        result += "::";
        result += cpp_identifier(components[i]);
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
        return join_cpp(split_components(
            absolute_proto_name.substr(1), '.', "Protobuf type name"));
    }

    const std::string prefix = "." + package + ".";
    if (absolute_proto_name.compare(0, prefix.size(), prefix) == 0) {
        return join_cpp(split_components(
            absolute_proto_name.substr(prefix.size()), '.', "Protobuf type name"));
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
        if (has_reserved_identifier_form(component, i == 0)) {
            throw std::runtime_error(
                "Protobuf package component '" + component +
                "' uses a reserved C++ identifier form");
        }
        if (i == 0 && component == "std") {
            throw std::runtime_error(
                "Protobuf package cannot generate the global C++ namespace 'std'");
        }
    }
}

} // namespace easypb_codegen
