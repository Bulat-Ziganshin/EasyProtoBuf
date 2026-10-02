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

// Return true if `value` starts with `prefix`.
bool starts_with(str_view value, str_view prefix);

// Return true if `value` ends with `suffix`.
bool ends_with(str_view value, str_view suffix);

// Convert a signed 32-bit integer without relying on std::to_string, which is
// missing from some legacy MinGW libstdc++ implementations.
std::string number_to_string(int32_t value);

// Returns a version of 'str' where every occurrence of
// 'find' is substituted by 'replace'.
// - http://stackoverflow.com/questions/20406744/
std::string string_replace_all(
    str_view str,   // where to work
    str_view find,  // substitute 'find'
    str_view replace //      by 'replace'
);

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

    std::string result;
    const char* fmt = format_str.data();
    size_t fmt_size = format_str.size();
    size_t cur_arg = 0;    // the number of the next argument denoted by "{}"
    size_t start = 0;      // starting position of the current literal span in format_str

    for(size_t i=0; i+1 < fmt_size; i++)
    {
        // add to the result the literal string between the two arguments and then the next argument
        auto process_next_arg = [&] (size_t arg_num) {
            if(arg_num >= arg_list.size()) {
                throw std::runtime_error("Not enough arguments for myformat");
            }
            result += str_view(&fmt[start], i - start);
            result += arg[arg_num];
        };

        // "{}" in the format string is replaced by the next input argument
        if(fmt[i]=='{' && fmt[i+1]=='}')
        {
            process_next_arg(cur_arg++);
            start = i+2;
        }
        // "{\d}" in the format string is replaced by the input argument number #d
        else if(i+2<fmt_size && fmt[i]=='{' && isdigit(fmt[i+1]) && fmt[i+2]=='}')
        {
            process_next_arg(atoi(&fmt[i+1]));
            start = i+3;
        }
    }

    // add to the result the remainder of format_str
    result += str_view(&fmt[start], fmt_size - start);

    return result;
}

#endif
