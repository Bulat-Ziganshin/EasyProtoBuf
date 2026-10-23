#ifndef EASYPB_CODEGEN_UTILS_HPP_INCLUDED
#define EASYPB_CODEGEN_UTILS_HPP_INCLUDED

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include "descriptor.pb.hpp"

// The generator's own text helpers. W-55: they used to live in the global
// namespace, and codegen/ is a PUBLIC include path of easypb_codegen_core, so
// every consumer that linked the core inherited names as generic as
// starts_with. The include graph is unchanged; only the names owners are.
namespace easypb_codegen {

// Return true if `value` starts with `prefix`.
bool starts_with(str_view value, str_view prefix);

// Return true if `value` ends with `suffix`.
bool ends_with(str_view value, str_view suffix);

// Convert a signed 32-bit integer without relying on std::to_string, which is
// missing from some legacy MinGW libstdc++ implementations.
std::string number_to_string(int32_t value);

} // namespace easypb_codegen

// Deliberately still global: this helper is dead and S7e's W-53 deletes it, so
// W-55 does not move what is about to disappear.
//
// Returns a version of 'str' where every occurrence of
// 'find' is substituted by 'replace'.
// - http://stackoverflow.com/questions/20406744/
std::string string_replace_all(
    str_view str,   // where to work
    str_view find,  // substitute 'find'
    str_view replace //      by 'replace'
);

namespace easypb_codegen {

// Use format_str to format remaining arguments similar to std::format.
// All arguments should be convertible to str_view, and the only
// formatting templates supported are {} and {\d}.
// Arguments are taken by reference on purpose. The by-value pack copied every
// lvalue argument into the parameter first - and the callers pass exactly the
// strings that must not be copied: the generator's own accumulation buffers
// (`nested_defs`, `field_defs`, `encoder`, `decoder`, `check_required_fields`)
// and its long qualified names.
template <typename... Args>
std::string myformat(str_view format_str, const Args&... args)
{
    std::initializer_list<str_view> arg_list{args...};
    auto arg = arg_list.begin();

    const char* fmt = format_str.data();
    size_t fmt_size = format_str.size();
    size_t reserve_size = fmt_size;
    for (const auto& item : arg_list) {
        reserve_size += item.size();
    }

    std::string result;
    result.reserve(reserve_size);
    size_t cur_arg = 0;    // the number of the next argument denoted by "{}"
    size_t start = 0;      // starting position of the current literal span in format_str

    for(size_t i=0; i+1 < fmt_size; i++)
    {
        // add to the result the literal string between the two arguments and then the next argument
        auto process_next_arg = [&] (size_t arg_num) {
            if(arg_num >= arg_list.size()) {
                throw std::runtime_error("Not enough arguments for myformat");
            }
            if (i > start) {
                result.append(fmt + start, i - start);
            }
            // W-35: an empty argument may be a null view, and append(nullptr, 0)
            // is formally undefined; the empty argument contributes nothing.
            if (!arg[arg_num].empty()) {
                result.append(arg[arg_num].data(), arg[arg_num].size());
            }
        };

        // "{}" in the format string is replaced by the next input argument
        if(fmt[i]=='{' && fmt[i+1]=='}')
        {
            process_next_arg(cur_arg++);
            start = i+2;
            i += 1;
        }
        // "{\d}" in the format string is replaced by the input argument number #d
        else if(i+2<fmt_size && fmt[i]=='{' && isdigit(static_cast<unsigned char>(fmt[i+1])) && fmt[i+2]=='}')
        {
            process_next_arg(static_cast<size_t>(fmt[i+1] - '0'));
            start = i+3;
            i += 2;
        }
    }

    // add to the result the remainder of format_str
    if (start < fmt_size) {
        result.append(fmt + start, fmt_size - start);
    }

    return result;
}

} // namespace easypb_codegen

#endif
