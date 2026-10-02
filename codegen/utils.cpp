#include "utils.hpp"

// Return true if `value` starts with `prefix`.
bool starts_with(str_view value, str_view prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

// Return true if `value` ends with `suffix`.
bool ends_with(str_view value, str_view suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Convert a signed 32-bit integer without relying on std::to_string, which is
// missing from some legacy MinGW libstdc++ implementations.
std::string number_to_string(int32_t value)
{
    char buffer[11];  // INT32_MIN has 10 decimal digits plus the minus sign.
    char* end = buffer + sizeof(buffer);
    char* begin = end;
    const bool negative = value < 0;
    uint32_t magnitude = negative
                       ? uint32_t(-(value + 1)) + 1
                       : uint32_t(value);

    do {
        *--begin = char('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);

    if (negative) {
        *--begin = '-';
    }

    return std::string(begin, end);
}

// Returns a version of 'str' where every occurrence of
// 'find' is substituted by 'replace'.
// - http://stackoverflow.com/questions/20406744/
std::string string_replace_all(
    str_view str ,   // where to work
    str_view find ,  // substitute 'find'
    str_view replace //      by 'replace'
) {
    using namespace std;
    string result;
    size_t find_len = find.size();
    size_t pos, from = 0;
    while( string::npos != ( pos=str.find(find,from) ) ) {
        result.append( str, from, pos-from );
        result.append( replace );
        from = pos + find_len;
    }
    result.append( str, from , string::npos );
    return result;
}
