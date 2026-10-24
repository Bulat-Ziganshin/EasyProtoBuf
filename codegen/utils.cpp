#include "utils.hpp"

namespace easypb_codegen {

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

} // namespace easypb_codegen
