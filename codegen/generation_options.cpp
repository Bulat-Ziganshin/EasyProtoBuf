#include "generation_options.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "utils.hpp"

namespace easypb_codegen {

namespace {

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

bool is_open_bracket(char c)
{
    return c == '<' || c == '(' || c == '[' || c == '{';
}

bool is_close_bracket(char c)
{
    return c == '>' || c == ')' || c == ']' || c == '}';
}

char matching_close(char open)
{
    switch (open) {
        case '<': return '>';
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        default: return '\0';
    }
}

// "1" for the first character of the value, so the position reads the way an
// editor counts columns.
std::string at_position(std::size_t index)
{
    return " at position " +
           number_to_string(static_cast<std::int32_t>(index + 1));
}

// The common prefix of every template diagnostic: the option and the spelling
// the caller typed.
std::string quoted_option(const char* option, const std::string& value)
{
    return std::string(option) + " \"" + value + "\": ";
}

// One validator for the `--repeated-type` and `--map-type` templates (V-10).
//
// The emitter substitutes placeholders with myformat and writes the result
// into the generated header, so a misspelled template becomes a header that
// does not compile while the tool exits 0. The grammar is therefore checked
// where the option is parsed:
//
//   * brackets nest and match: `< >`, `( )`, `[ ]`, `{ }`;
//   * the template is one type expression - after the outermost group closes,
//     only whitespace may follow;
//   * `{{` and `}}` are refused: myformat has no escape syntax, so a doubled
//     brace would reach the header as one literal brace;
//   * every argument the option supplies is used at least once. `{}` is the
//     next argument and `{N}` argument N, exactly as myformat reads them; a
//     placeholder past the last argument is refused.
//
// `spelling` is what the caller wrote; `normalized` is that spelling with the
// default placeholders appended when it had none. The structure is checked on
// the spelling (the append cannot repair a broken bracket), the placeholders
// on the normalized value, and every diagnostic quotes the spelling.
bool validate_container_template(const char* option, const std::string& spelling,
                                 const std::string& normalized,
                                 const char* const* argument_names,
                                 std::size_t argument_count,
                                 std::string& error)
{
    const std::size_t size = spelling.size();

    for (std::size_t i = 0; i + 1 < size; ++i) {
        if ((spelling[i] == '{' || spelling[i] == '}') &&
            spelling[i + 1] == spelling[i]) {
            error = quoted_option(option, spelling) + "a doubled '" +
                    std::string(1, spelling[i]) + "'" + at_position(i) +
                    " is not a placeholder escape";
            return false;
        }
    }

    std::vector<char> opens;
    std::vector<std::size_t> opened_at;
    std::size_t outermost_close = std::string::npos;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = spelling[i];
        if (is_open_bracket(c)) {
            opens.push_back(c);
            opened_at.push_back(i);
            continue;
        }
        if (is_close_bracket(c)) {
            if (opens.empty()) {
                error = quoted_option(option, spelling) + "'" +
                        std::string(1, c) + "'" + at_position(i) +
                        " has no matching opening bracket";
                return false;
            }
            if (matching_close(opens.back()) != c) {
                error = quoted_option(option, spelling) + "'" +
                        std::string(1, c) + "'" + at_position(i) +
                        " does not close the '" +
                        std::string(1, opens.back()) + "'" +
                        at_position(opened_at.back());
                return false;
            }
            opens.pop_back();
            opened_at.pop_back();
            if (opens.empty()) outermost_close = i;
            continue;
        }
        if (outermost_close != std::string::npos && !is_space(c)) {
            error = quoted_option(option, spelling) + "'" + std::string(1, c) +
                    "'" + at_position(i) +
                    " follows the end of the template";
            return false;
        }
    }
    if (!opens.empty()) {
        error = quoted_option(option, spelling) + "'" +
                std::string(1, opens.back()) + "'" +
                at_position(opened_at.back()) + " is never closed";
        return false;
    }

    // The placeholders, read the way myformat reads them. A '{' that does not
    // open a placeholder is a brace group the structural pass already checked.
    std::vector<bool> used(argument_count, false);
    std::size_t next_argument = 0;
    const std::size_t value_size = normalized.size();
    for (std::size_t i = 0; i < value_size; ++i) {
        if (normalized[i] != '{') continue;
        std::size_t end = i + 1;
        bool numbered = false;
        std::size_t index = 0;
        while (end < value_size && normalized[end] >= '0' &&
               normalized[end] <= '9') {
            numbered = true;
            // Any placeholder this large is past every argument count this
            // option can have; cap it so the arithmetic cannot wrap.
            if (index < 100000) {
                index = index * 10 +
                        static_cast<std::size_t>(normalized[end] - '0');
            }
            ++end;
        }
        if (!numbered) {
            if (end < value_size && normalized[end] == '}') {
                index = next_argument++;
            } else {
                continue;
            }
        } else if (end >= value_size || normalized[end] != '}') {
            continue;
        }
        const std::size_t placeholder_at = i;
        i = end;
        if (index >= argument_count) {
            error = quoted_option(option, spelling) + "argument " +
                    number_to_string(static_cast<std::int32_t>(index + 1)) +
                    at_position(placeholder_at) +
                    " is past the last argument";
            return false;
        }
        used[index] = true;
    }

    for (std::size_t argument = 0; argument < argument_count; ++argument) {
        if (used[argument]) continue;
        error = quoted_option(option, spelling) + "the template does not use " +
                argument_names[argument] + " (argument " +
                number_to_string(static_cast<std::int32_t>(argument + 1)) + ")";
        return false;
    }

    return true;
}

} // namespace

bool finalize_generation_options(GenerationOptions& options, std::string& error)
{
    // W-50: the contradiction is refused BEFORE any normalization, so a caller
    // that keeps using its object after a false return reads the options it
    // passed, not a half-normalized mix. The success path still normalizes in
    // place, exactly as the header documents.
    if (options.packed && options.no_packed) {
        error = "Options --packed and --no-packed can't be used together";
        return false;
    }
    // A required field's presence cannot be checked without its has_* field,
    // so suppressing the latter suppresses the check as well.
    GenerationOptions normalized = options;
    if (normalized.no_has_fields) {
        normalized.no_required = true;
    }
    if (options.cpp_repeated_type.find("{}") == std::string::npos &&
        options.cpp_repeated_type.find("{0}") == std::string::npos) {
        normalized.cpp_repeated_type += "<{}>";
    }
    if (options.cpp_map_type.find("{}") == std::string::npos &&
        options.cpp_map_type.find("{0}") == std::string::npos &&
        options.cpp_map_type.find("{1}") == std::string::npos) {
        normalized.cpp_map_type += "<{0},{1}>";
    }

    // V-10: a template the emitter cannot substitute is a header that does not
    // compile, so it is refused here, while the caller can still be told which
    // option and which character are wrong. The failure leaves `options`
    // untouched, like the contradiction above.
    static const char* const repeated_arguments[] = { "the element type" };
    static const char* const map_arguments[] = { "the key type",
                                                 "the value type" };
    if (!validate_container_template(
            "--repeated-type", options.cpp_repeated_type,
            normalized.cpp_repeated_type, repeated_arguments, 1, error)) {
        return false;
    }
    if (!validate_container_template(
            "--map-type", options.cpp_map_type, normalized.cpp_map_type,
            map_arguments, 2, error)) {
        return false;
    }

    options = normalized;
    return true;
}

} // namespace easypb_codegen
