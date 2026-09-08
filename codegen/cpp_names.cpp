#include "cpp_names.hpp"

#include <cctype>
#include <set>
#include <stdexcept>
#include <vector>

namespace easypb_codegen {
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

const std::set<std::string>& cpp_keywords()
{
    static const char* const values[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel",
        "atomic_commit", "atomic_noexcept", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
        "char32_t", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do",
        "double", "dynamic_cast", "else", "enum", "explicit", "export",
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

std::string cpp_absolute_name(const std::string& absolute_proto_name)
{
    if (absolute_proto_name.size() < 2 || absolute_proto_name[0] != '.') {
        throw std::runtime_error(
            "absolute Protobuf type name must begin with '.'");
    }

    const std::vector<std::string> components =
        split_components(absolute_proto_name.substr(1), '.', "Protobuf type name");
    return "::" + join_cpp(components);
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

    std::string local_proto_name;
    if (package.empty()) {
        local_proto_name = absolute_proto_name.substr(1);
    } else {
        const std::string prefix = "." + package + ".";
        if (absolute_proto_name.compare(0, prefix.size(), prefix) != 0) {
            throw std::runtime_error(
                "insertion macro message name does not belong to the current package");
        }
        local_proto_name = absolute_proto_name.substr(prefix.size());
    }

    const std::vector<std::string> components =
        split_components(local_proto_name, '.', "Protobuf message name");
    std::string result = "EASYPB_";
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i != 0) result += "_";
        result += components[i];
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
        if (cpp_keywords().count(component) != 0) {
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
