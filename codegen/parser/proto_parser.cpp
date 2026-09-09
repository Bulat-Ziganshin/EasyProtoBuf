#include "proto_parser.hpp"
#include "schema_semantics.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

/*
 * Grammar-to-code guide
 * =====================
 *
 * The formal grammar implemented by this file is in
 * grammar/easypb-parser.peg.  The recursive-descent methods below use
 * the same rule names wherever practical.  This index is the quickest way to
 * navigate from a .proto construct to its implementation.
 *
 * Lexical rules (class Lexer)
 * ---------------------------
 *   %whitespace, Space, LineComment, BlockComment -> skip_space_and_comments()
 *   Identifier                                  -> identifier()
 *   IntegerToken, FloatToken, Exponent          -> number()
 *   StringAtom, Escape and all escape subrules  -> string_literal()
 *   punctuation and end-of-input                -> next()
 *
 * Syntactic rules (class Parser)
 * ------------------------------
 *   ProtoFile            -> parse()
 *   SyntaxStatement      -> parse_syntax()
 *   Package              -> parse_package()
 *   Import               -> parse_import()
 *   OptionStatement      -> parse_option_statement()
 *   Service              -> parse_service()
 *   Rpc                  -> parse_rpc()
 *   RpcBody              -> parse_rpc_body()
 *   OptionName           -> option_name()
 *   OptionNamePart       -> option_name_part()
 *   Constant             -> constant()
 *   StringSequence       -> string_sequence()
 *   FullIdentifier,
 *   TypeName             -> full_identifier()
 *   FieldOptions         -> field_options()
 *   FieldNumber          -> positive_field_number()
 *   FieldRangeNumber     -> field_range_number()
 *   SignedInteger        -> signed_enum_number() when used by enums/ranges
 *   Field, OneofField    -> parse_field()
 *   MapField             -> parse_map()
 *   Message              -> parse_message()
 *   Oneof                -> parse_oneof()
 *   Enum, EnumValue      -> parse_enum()
 *   ReservedMessage,
 *   ReservedEnum         -> parse_reserved()
 *   Extensions           -> parse_extensions()
 *   Extend               -> parse_extend()
 *
 * Semantic rules (not expressible by PEG alone)
 * ------------------------------------------------
 *   scalar/custom type split                     -> set_type()
 *   descriptor representation of default values  -> apply_default()
 *   reserved/extension conflicts                 -> validate_message_constraints()
 *   symbol indexing, lexical name lookup,        -> easypb_schema::index_file(),
 *   type-dependent default and packed checks        easypb_schema::resolve_file()
 *      (schema_semantics.hpp/.cpp, shared with the future linker)
 *   source positions and deferred output         -> FieldSource,
 *                                                    declaration_locations
 *
 * The parser intentionally constructs FileDescriptorProto directly rather
 * than building an intermediate AST.  Each parse_* method consumes one
 * grammar production from current_ and leaves current_ at the first token
 * following that production.  Declaration order defines descriptor indexes,
 * so every parse_* method that appends to a repeated descriptor field knows
 * the appended element's descriptor path (see schema.hpp) and records it in
 * field_sources/declaration_locations immediately.
 */

namespace easypb_proto {

typedef easypb_schema::DescriptorPath DescriptorPath;
typedef easypb_schema::FieldSource FieldSource;

namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

} // namespace

namespace {

enum TokenKind {
    TOKEN_END,
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER,
    TOKEN_FLOAT,
    TOKEN_STRING,
    TOKEN_SYMBOL
};

struct Token
{
    TokenKind kind;
    std::string text;
    char symbol;
    SourceLocation location;

    Token() : kind(TOKEN_END), symbol(0) {}
};

class ParseFailure : public std::runtime_error
{
public:
    SourceLocation location;

    ParseFailure(const SourceLocation& where, const std::string& message)
        : std::runtime_error(message), location(where) {}
};

bool ascii_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool ascii_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool ascii_hex(char c)
{
    return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

void append_utf8(std::string& output, unsigned long codepoint,
                 const SourceLocation& location)
{
    if (codepoint > 0x10fffful || (codepoint >= 0xd800ul && codepoint <= 0xdffful)) {
        throw ParseFailure(location, "invalid Unicode escape value");
    }
    if (codepoint <= 0x7ful) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7fful) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xfffful) {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}


/*
 * Lexer for the token-level rules at the bottom of
 * grammar/easypb-parser.peg.
 *
 * Keywords are deliberately returned as TOKEN_IDENTIFIER.  Parser::is_keyword
 * decides whether a particular identifier acts as a keyword in its current
 * grammar position.  This preserves legal identifiers such as "message_type"
 * and permits context-sensitive cases such as a user type named "map".
 */
class Lexer
{
public:
    Lexer(const char* source, std::size_t source_size)
        : source_(source), source_size_(source_size), offset_(0), line_(1), column_(1) {}

    // Lexical dispatcher: whitespace* (Identifier / IntegerToken /
    // FloatToken / StringAtom / punctuation / end-of-input).
    Token next()
    {
        skip_space_and_comments();
        Token token;
        token.location = location();
        if (offset_ == source_size_) {
            token.kind = TOKEN_END;
            return token;
        }

        const char c = peek();
        if (ascii_alpha(c) || c == '_') return identifier();
        if (ascii_digit(c) || (c == '.' && ascii_digit(peek(1)))) return number();
        if (c == '\'' || c == '"') return string_literal();

        static const char* punctuation = "{}[]()<>=;,.+-:";
        if (std::strchr(punctuation, c) != 0) {
            token.kind = TOKEN_SYMBOL;
            token.symbol = c;
            token.text.assign(1, c);
            advance();
            return token;
        }

        std::ostringstream message;
        message << "unexpected character 0x" << std::hex
                << static_cast<unsigned>(static_cast<unsigned char>(c));
        throw ParseFailure(token.location, message.str());
    }

private:
    const char* source_;
    std::size_t source_size_;
    std::size_t offset_;
    std::size_t line_;
    std::size_t column_;

    SourceLocation location() const
    {
        SourceLocation result;
        result.offset = offset_;
        result.line = line_;
        result.column = column_;
        return result;
    }

    char peek(std::size_t ahead = 0) const
    {
        const std::size_t position = offset_ + ahead;
        return position < source_size_ ? source_[position] : '\0';
    }

    void advance()
    {
        if (offset_ >= source_size_) return;
        const char c = source_[offset_++];
        if (c == '\r') {
            if (offset_ < source_size_ && source_[offset_] == '\n') ++offset_;
            ++line_;
            column_ = 1;
        } else if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
    }

    // %whitespace <- (Space / LineComment / BlockComment)*
    void skip_space_and_comments()
    {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n' ||
                   peek() == '\f' || peek() == '\v') {
                advance();
            }
            if (peek() == '/' && peek(1) == '/') {
                advance();
                advance();
                while (peek() != '\0' && peek() != '\r' && peek() != '\n') advance();
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                const SourceLocation start = location();
                advance();
                advance();
                while (!(peek() == '*' && peek(1) == '/')) {
                    if (peek() == '\0') throw ParseFailure(start, "unterminated block comment");
                    advance();
                }
                advance();
                advance();
                continue;
            }
            break;
        }
    }

    // Identifier <- [A-Za-z_] [A-Za-z0-9_]*
    Token identifier()
    {
        Token token;
        token.kind = TOKEN_IDENTIFIER;
        token.location = location();
        const std::size_t start = offset_;
        advance();
        while (ascii_alpha(peek()) || ascii_digit(peek()) || peek() == '_') advance();
        token.text.assign(source_ + start, offset_ - start);
        return token;
    }

    // IntegerToken / FloatToken.  A leading sign is a parser token,
    // not part of the lexical token, so Constant and SignedInteger consume it.
    Token number()
    {
        Token token;
        token.location = location();
        const std::size_t start = offset_;
        bool floating = false;

        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance();
            advance();
            const std::size_t digits = offset_;
            while (ascii_hex(peek())) advance();
            if (offset_ == digits) throw ParseFailure(token.location, "hex literal has no digits");
            if (ascii_alpha(peek()) || ascii_digit(peek()) || peek() == '_') {
                throw ParseFailure(token.location, "invalid suffix on hexadecimal integer literal");
            }
            token.kind = TOKEN_INTEGER;
            token.text.assign(source_ + start, offset_ - start);
            return token;
        }

        if (peek() == '.') {
            floating = true;
            advance();
            while (ascii_digit(peek())) advance();
        } else {
            while (ascii_digit(peek())) advance();
            if (peek() == '.') {
                floating = true;
                advance();
                while (ascii_digit(peek())) advance();
            }
        }

        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            const std::size_t digits = offset_;
            while (ascii_digit(peek())) advance();
            if (offset_ == digits) throw ParseFailure(token.location, "exponent has no digits");
        }

        if (ascii_alpha(peek()) || peek() == '_') {
            throw ParseFailure(token.location, "invalid suffix on numeric literal");
        }
        token.kind = floating ? TOKEN_FLOAT : TOKEN_INTEGER;
        token.text.assign(source_ + start, offset_ - start);
        return token;
    }

    // StringAtom plus Escape, HexEscape, OctalEscape, UnicodeEscape
    // and UnicodeLongEscape.  token.text contains decoded bytes, without quotes.
    Token string_literal()
    {
        Token token;
        token.kind = TOKEN_STRING;
        token.location = location();
        const char quote = peek();
        advance();

        while (peek() != quote) {
            if (peek() == '\0') throw ParseFailure(token.location, "unterminated string literal");
            if (peek() == '\r' || peek() == '\n') {
                throw ParseFailure(location(), "newline in string literal");
            }
            if (peek() != '\\') {
                token.text.push_back(peek());
                advance();
                continue;
            }

            const SourceLocation escape_location = location();
            advance();
            const char escaped = peek();
            if (escaped == '\0') throw ParseFailure(escape_location, "unterminated escape sequence");
            advance();
            switch (escaped) {
                case 'a': token.text.push_back('\a'); break;
                case 'b': token.text.push_back('\b'); break;
                case 'f': token.text.push_back('\f'); break;
                case 'n': token.text.push_back('\n'); break;
                case 'r': token.text.push_back('\r'); break;
                case 't': token.text.push_back('\t'); break;
                case 'v': token.text.push_back('\v'); break;
                case '\\': token.text.push_back('\\'); break;
                case '\'': token.text.push_back('\''); break;
                case '"': token.text.push_back('"'); break;
                case '?': token.text.push_back('?'); break;
                case 'x': {
                    if (!ascii_hex(peek())) {
                        throw ParseFailure(escape_location, "\\x escape has no hexadecimal digits");
                    }
                    unsigned value = 0;
                    unsigned count = 0;
                    while (count != 2 && ascii_hex(peek())) {
                        value = value * 16u + static_cast<unsigned>(hex_value(peek()));
                        advance();
                        ++count;
                    }
                    token.text.push_back(static_cast<char>(value));
                    break;
                }
                case 'u':
                case 'U': {
                    const unsigned digits = escaped == 'u' ? 4u : 8u;
                    unsigned long value = 0;
                    for (unsigned i = 0; i < digits; ++i) {
                        if (!ascii_hex(peek())) {
                            throw ParseFailure(escape_location, "incomplete Unicode escape");
                        }
                        value = value * 16ul + static_cast<unsigned long>(hex_value(peek()));
                        advance();
                    }
                    append_utf8(token.text, value, escape_location);
                    break;
                }
                default:
                    if (escaped >= '0' && escaped <= '7') {
                        unsigned value = static_cast<unsigned>(escaped - '0');
                        unsigned count = 1;
                        while (count != 3 && peek() >= '0' && peek() <= '7') {
                            value = value * 8u + static_cast<unsigned>(peek() - '0');
                            advance();
                            ++count;
                        }
                        if (value > 255u) throw ParseFailure(escape_location, "octal escape exceeds one byte");
                        token.text.push_back(static_cast<char>(value));
                    } else {
                        throw ParseFailure(escape_location, "unknown escape sequence");
                    }
                    break;
            }
        }
        advance();
        return token;
    }
};

// The shared helpers parse_unsigned_integer, parse_signed_32, is_float_text
// and the scalar-type classifiers (builtin_type, packable_type,
// valid_map_key_type) live in schema_semantics.hpp/.cpp so the parser and
// the shared semantic passes classify types and validate scalar defaults
// identically. The 64-bit default-range helpers stay private to the semantic
// module: default range validation moved there with validate_default.

// FileDescriptorProto stores floating defaults in the same canonical form as
// protobuf's SimpleFtoa/SimpleDtoa: first try a non-over-precise decimal
// representation, then fall back to enough digits for exact round-tripping.
std::string normalize_exponent(std::string text)
{
    const std::size_t marker = text.find_first_of("eE");
    if (marker == std::string::npos) return text;

    std::size_t first_digit = marker + 1;
    if (first_digit < text.size() &&
        (text[first_digit] == '+' || text[first_digit] == '-')) {
        ++first_digit;
    }

    // Older MSVCRT versions emit three-digit exponents such as e-008.
    // Protobuf keeps at least two digits but drops redundant leading zeros.
    while (first_digit + 2 < text.size() && text[first_digit] == '0') {
        text.erase(first_digit, 1);
    }
    return text;
}

// Format floating-point values only.  Exponent normalization assumes that an
// 'e' or 'E' in the stream output starts a scientific-notation exponent.
std::string format_FP(double value, int precision)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(precision) << value;
    return normalize_exponent(output.str());
}

std::string canonical_float_default(const std::string& text, bool single_precision)
{
    std::size_t start = 0;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) start = 1;
    const std::string body = text.substr(start);
    if (body == "inf") return (!text.empty() && text[0] == '-') ? "-inf" : "inf";
    if (body == "nan") return "nan";

    char* end = 0;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) return text;

    if (single_precision) {
        const float value = static_cast<float>(parsed);
        std::string result = format_FP(value, std::numeric_limits<float>::digits10);
        char* parsed_end = 0;
        const float round_trip = static_cast<float>(std::strtod(result.c_str(), &parsed_end));
        if (parsed_end == result.c_str() || *parsed_end != '\0' || round_trip != value) {
            result = format_FP(value, std::numeric_limits<float>::digits10 + 3);
        }
        return result;
    }

    std::string result = format_FP(parsed, std::numeric_limits<double>::digits10);
    char* parsed_end = 0;
    const double round_trip = std::strtod(result.c_str(), &parsed_end);
    if (parsed_end == result.c_str() || *parsed_end != '\0' || round_trip != parsed) {
        result = format_FP(parsed, std::numeric_limits<double>::digits10 + 2);
    }
    return result;
}

std::string escape_bytes(const std::string& bytes)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned c = static_cast<unsigned char>(bytes[i]);
        switch (c) {
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            case '\\': output << "\\\\"; break;
            case '\'': output << "\\'"; break;
            case '"': output << "\\\""; break;
            default:
                if (c >= 32u && c <= 126u) output << static_cast<char>(c);
                else {
                    output << '\\'
                           << static_cast<char>('0' + ((c >> 6) & 7u))
                           << static_cast<char>('0' + ((c >> 3) & 7u))
                           << static_cast<char>('0' + (c & 7u));
                }
                break;
        }
    }
    return output.str();
}

std::string camel_case(const std::string& input)
{
    std::string output;
    bool capitalize = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '_') {
            capitalize = true;
            continue;
        }
        if (capitalize && c >= 'a' && c <= 'z') output.push_back(static_cast<char>(c - 'a' + 'A'));
        else output.push_back(c);
        capitalize = false;
    }
    if (output.empty()) output = "Map";
    return output;
}

struct Constant
{
    enum Kind {
        STRING_VALUE,
        INTEGER_VALUE,
        FLOAT_VALUE,
        BOOL_VALUE,
        IDENTIFIER_VALUE,
        AGGREGATE_VALUE
    };

    Kind kind;
    std::string text;
    SourceLocation location;
};

struct FieldOptionState
{
    bool has_default;
    Constant default_value;
    bool has_packed;
    bool packed;
    SourceLocation packed_location;
    bool has_cpp_type;
    std::string cpp_type;

    FieldOptionState()
        : has_default(false), has_packed(false), packed(false),
          has_cpp_type(false) {}
};

// Source provenance captured while parsing one field. The caller records it
// as a FieldSource once the field's descriptor index (and therefore its
// descriptor path) is known.
struct FieldProvenance
{
    std::string raw_type_name;
    SourceLocation type_location;
    SourceLocation name_location;
    bool has_default;
    SourceLocation default_location;
    bool has_packed;
    SourceLocation packed_location;

    FieldProvenance() : has_default(false), has_packed(false) {}
};

struct NumberRange
{
    std::int64_t first;
    std::int64_t last;
    SourceLocation location;

    NumberRange() : first(0), last(0) {}
};

bool range_contains(const NumberRange& range, std::int64_t value)
{
    return value >= range.first && value <= range.last;
}

bool ranges_overlap(const NumberRange& left, const NumberRange& right)
{
    return left.first <= right.last && right.first <= left.last;
}

class Parser
{
public:
    Parser(const std::string& file_name, const char* source, std::size_t source_size,
           ParsedProto& result, const ParseOptions& options)
        : file_name_(file_name), lexer_(source, source_size), out_(result),
          seen_syntax_(false), seen_package_(false), seen_statement_(false),
          defer_type_resolution_(options.defer_type_resolution)
    {
        current_ = lexer_.next();
        next_ = lexer_.next();
    }

    // ProtoFile <- EmptyStatement* SyntaxStatement? TopLevel* !.
    // Dispatches TopLevel alternatives and then runs the shared semantic
    // passes (easypb_schema::index_file for both modes, plus
    // easypb_schema::resolve_file for validation).
    void parse()
    {
        out_.file.name = out_.strings.save(file_name_);
        out_.file.has_name = true;
        out_.physical_name = file_name_;
        out_.from_descriptor_set = false;

        while (current_.kind != TOKEN_END) {
            // EmptyStatement
            if (accept_symbol(';')) continue;

            // SyntaxStatement is special: unlike TopLevel, it may occur only
            // once and only before every non-empty top-level statement.
            if (is_keyword("syntax")) {
                if (seen_statement_ || seen_syntax_) fail("syntax declaration must be the first statement");
                parse_syntax();
                seen_syntax_ = true;
                continue;
            }
            seen_statement_ = true;

            // TopLevel <- Package / Import / OptionStatement / Message /
            //             Enum / Extend / Service
            if (accept_keyword("package")) parse_package();
            else if (accept_keyword("import")) parse_import();
            else if (accept_keyword("option")) parse_option_statement();
            else if (accept_keyword("message")) {
                const SourceLocation name_location = current_.location;
                const DescriptorPath path = easypb_schema::file_message_path(
                    static_cast<int>(out_.file.message_type.size()));
                out_.file.message_type.push_back(
                    parse_message(path));
                out_.declaration_locations[path] = name_location;
            }
            else if (accept_keyword("enum")) {
                const SourceLocation name_location = current_.location;
                const DescriptorPath path = easypb_schema::file_enum_path(
                    static_cast<int>(out_.file.enum_type.size()));
                out_.file.enum_type.push_back(
                    parse_enum(path));
                out_.declaration_locations[path] = name_location;
            }
            else if (accept_keyword("extend")) parse_extend();
            else if (accept_keyword("service")) parse_service();
            else fail("expected a top-level .proto statement");
        }

        run_semantic_passes();
    }

private:
    std::string file_name_;
    Lexer lexer_;
    ParsedProto& out_;
    Token current_;
    Token next_;
    bool seen_syntax_;
    bool seen_package_;
    bool seen_statement_;
    bool defer_type_resolution_;

    void advance()
    {
        current_ = next_;
        next_ = lexer_.next();
    }

    bool next_is_symbol(char symbol) const
    {
        return next_.kind == TOKEN_SYMBOL && next_.symbol == symbol;
    }

    bool is_keyword(const char* keyword) const
    {
        return current_.kind == TOKEN_IDENTIFIER && current_.text == keyword;
    }

    bool accept_keyword(const char* keyword)
    {
        if (!is_keyword(keyword)) return false;
        advance();
        return true;
    }

    bool accept_symbol(char symbol)
    {
        if (current_.kind != TOKEN_SYMBOL || current_.symbol != symbol) return false;
        advance();
        return true;
    }

    void expect_symbol(char symbol)
    {
        if (!accept_symbol(symbol)) {
            std::string message = "expected '";
            message.push_back(symbol);
            message.push_back('\'');
            fail(message);
        }
    }

    void fail(const std::string& message) const
    {
        throw ParseFailure(current_.location, message);
    }

    // Identifier.  The lexer already validated its spelling.
    std::string identifier()
    {
        if (current_.kind != TOKEN_IDENTIFIER) fail("expected identifier");
        const std::string result = current_.text;
        advance();
        return result;
    }

    // FullIdentifier <- Identifier ("." Identifier)*
    // TypeName adds the optional leading dot when allow_leading_dot is true.
    std::string full_identifier(bool allow_leading_dot)
    {
        std::string result;
        if (allow_leading_dot && accept_symbol('.')) result = ".";
        result += identifier();
        while (accept_symbol('.')) {
            result += ".";
            result += identifier();
        }
        return result;
    }

    // StringSequence <- StringAtom+
    // Adjacent quoted literals are concatenated after each atom is decoded.
    std::string string_sequence()
    {
        if (current_.kind != TOKEN_STRING) fail("expected string literal");
        std::string result;
        do {
            result += current_.text;
            advance();
        } while (current_.kind == TOKEN_STRING);
        return result;
    }

    // SyntaxStatement <- "syntax" "=" StringSequence ";"
    void parse_syntax()
    {
        accept_keyword("syntax");
        expect_symbol('=');
        const std::string value = string_sequence();
        if (value != "proto2" && value != "proto3") {
            fail("syntax must be \"proto2\" or \"proto3\"");
        }
        expect_symbol(';');
        // protoc represents proto2 by leaving FileDescriptorProto.syntax
        // absent; only proto3 is materialized in field 12.
        if (value == "proto3") {
            out_.file.syntax = out_.strings.save(value);
            out_.file.has_syntax = true;
        }
    }

    // Package <- "package" FullIdentifier ";"
    void parse_package()
    {
        if (seen_package_) fail("duplicate package declaration");
        const SourceLocation name_location = current_.location;
        const std::string name = full_identifier(false);
        expect_symbol(';');
        out_.file.package = out_.strings.save(name);
        out_.file.has_package = true;
        // The package name lives at FileDescriptorProto field 2, a singular
        // field addressed by the one-element path [2]. The position serves
        // cross-file package-collision diagnostics during indexing.
        DescriptorPath package_path;
        package_path.push_back(2);
        out_.declaration_locations[package_path] = name_location;
        seen_package_ = true;
    }

    // Import <- "import" ("public" / "weak")? StringSequence ";"
    // Imports are retained in both ParsedProto::imports (with positions) and
    // the authoritative FileDescriptorProto dependency lists; files are not
    // loaded here.
    void parse_import()
    {
        ImportInfo info;
        info.location = current_.location;
        if (accept_keyword("public")) info.modifier = ImportInfo::PUBLIC_IMPORT;
        else if (accept_keyword("weak")) info.modifier = ImportInfo::WEAK_IMPORT;
        info.path = string_sequence();
        expect_symbol(';');
        out_.imports.push_back(info);
        out_.file.dependency.push_back(out_.strings.save(info.path));
        const int index = static_cast<int>(out_.file.dependency.size() - 1);
        if (info.modifier == ImportInfo::PUBLIC_IMPORT) {
            out_.file.public_dependency.push_back(index);
        } else if (info.modifier == ImportInfo::WEAK_IMPORT) {
            out_.file.weak_dependency.push_back(index);
        }
    }

    // OptionNamePart <- Identifier / "(" "."? FullIdentifier ")"
    std::string option_name_part()
    {
        if (!accept_symbol('(')) return identifier();

        std::string identifier = full_identifier(true);
        if (!identifier.empty() && identifier[0] == '.') identifier.erase(0, 1);
        std::string part = "(" + identifier;
        expect_symbol(')');
        part += ")";
        return part;
    }

    // OptionName <- OptionNamePart ("." OptionNamePart)*
    std::string option_name()
    {
        std::string name = option_name_part();
        while (accept_symbol('.')) {
            name += ".";
            name += option_name_part();
        }
        return name;
    }

    // Constant <- StringSequence / SignedFloat / SignedInteger /
    //             BoolLiteral / FullIdentifier / AggregateValue
    Constant constant()
    {
        Constant value;
        value.location = current_.location;

        // StringSequence alternative.
        if (current_.kind == TOKEN_STRING) {
            value.kind = Constant::STRING_VALUE;
            value.text = string_sequence();
            return value;
        }
        // SignedInteger and SignedFloat share an optional leading Sign.
        std::string sign;
        if (current_.kind == TOKEN_SYMBOL && (current_.symbol == '+' || current_.symbol == '-')) {
            sign.assign(1, current_.symbol);
            advance();
        }
        if (current_.kind == TOKEN_INTEGER) {
            value.kind = Constant::INTEGER_VALUE;
            value.text = sign + current_.text;
            advance();
            return value;
        }
        if (current_.kind == TOKEN_FLOAT) {
            value.kind = Constant::FLOAT_VALUE;
            value.text = sign + current_.text;
            advance();
            return value;
        }
        // BoolLiteral, inf/nan, and FullIdentifier all arrive as identifier tokens.
        if (current_.kind == TOKEN_IDENTIFIER) {
            std::string text = full_identifier(false);
            if (!sign.empty() && text != "inf" && text != "nan") {
                fail("a sign is allowed only before a numeric literal, inf, or nan");
            }
            value.text = sign + text;
            if (sign.empty() && (text == "true" || text == "false")) value.kind = Constant::BOOL_VALUE;
            else if (text == "inf" || text == "nan") value.kind = Constant::FLOAT_VALUE;
            else value.kind = Constant::IDENTIFIER_VALUE;
            return value;
        }
        if (!sign.empty()) fail("expected value after sign");

        // AggregateValue.  The parser only balances braces because arbitrary options
        // are not represented by the trimmed descriptor model.
        if (accept_symbol('{')) {
            value.kind = Constant::AGGREGATE_VALUE;
            value.text = "{}";
            unsigned depth = 1;
            while (depth != 0) {
                if (current_.kind == TOKEN_END) fail("unterminated aggregate option value");
                if (current_.kind == TOKEN_SYMBOL && current_.symbol == '{') ++depth;
                else if (current_.kind == TOKEN_SYMBOL && current_.symbol == '}') --depth;
                advance();
            }
            return value;
        }
        fail("expected option constant");
        return value;
    }

    // OptionStatement <- "option" OptionName "=" Constant ";"
    // The trimmed descriptor model does not retain general options, so this
    // method validates and consumes them without storing them.
    void parse_option_statement()
    {
        (void)option_name();
        expect_symbol('=');
        (void)constant();
        expect_symbol(';');
    }

    // RpcBody <- "{" (EmptyStatement / OptionStatement)* "}"
    // RPC metadata is intentionally not retained by the trimmed descriptor.
    void parse_rpc_body()
    {
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated rpc body");
            if (accept_symbol(';')) continue;
            if (accept_keyword("option")) {
                parse_option_statement();
                continue;
            }
            fail("expected option or ';' in rpc body");
        }
    }

    // Rpc <- "rpc" Identifier "(" "stream"? TypeName ")"
    //        "returns" "(" "stream"? TypeName ")" (RpcBody / ";")
    // The caller has consumed "rpc". Request/response names and method
    // options are validated syntactically and then discarded.
    void parse_rpc()
    {
        (void)identifier();
        expect_symbol('(');
        (void)accept_keyword("stream");
        (void)full_identifier(true);
        expect_symbol(')');
        if (!accept_keyword("returns")) fail("expected 'returns' in rpc declaration");
        expect_symbol('(');
        (void)accept_keyword("stream");
        (void)full_identifier(true);
        expect_symbol(')');
        if (accept_symbol(';')) return;
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '{') {
            parse_rpc_body();
            return;
        }
        fail("expected ';' or rpc body");
    }

    // Service <- "service" Identifier "{" ServiceElement* "}"
    // ServiceElement <- EmptyStatement / OptionStatement / Rpc
    // Services are accepted so message schemas can be consumed by Codegen.
    // Only the service names are retained in the trimmed descriptor model
    // because EasyProtoBuf generates message codecs rather than RPC APIs;
    // the names still participate in package-scope collision validation.
    void parse_service()
    {
        const SourceLocation name_location = current_.location;
        const std::string name = identifier();
        ServiceDescriptorProto service;
        service.name = out_.strings.save(name);
        service.has_name = true;
        const DescriptorPath path = easypb_schema::file_service_path(
            static_cast<int>(out_.file.service.size()));
        out_.file.service.push_back(service);
        out_.declaration_locations[path] = name_location;
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated service body");
            if (accept_symbol(';')) continue;
            if (accept_keyword("option")) {
                parse_option_statement();
                continue;
            }
            if (accept_keyword("rpc")) {
                parse_rpc();
                continue;
            }
            fail("expected rpc, option, or ';' in service body");
        }
    }

    // FieldOptions <- "[" FieldOption ("," FieldOption)* "]"
    // Standard options used by Codegen plus the EasyProtoBuf C++ type option
    // are retained in the trimmed descriptor. Other options are still parsed
    // as OptionName "=" Constant and then ignored.
    FieldOptionState field_options()
    {
        FieldOptionState state;
        if (!accept_symbol('[')) return state;
        do {
            const SourceLocation option_location = current_.location;
            const std::string name = option_name();
            expect_symbol('=');
            const Constant value = constant();
            if (name == "default") {
                if (state.has_default) throw ParseFailure(option_location, "duplicate default field option");
                state.has_default = true;
                state.default_value = value;
            } else if (name == "packed") {
                if (state.has_packed) throw ParseFailure(option_location, "duplicate packed field option");
                if (value.kind != Constant::BOOL_VALUE) {
                    throw ParseFailure(value.location, "packed option must be true or false");
                }
                state.has_packed = true;
                state.packed = value.text == "true";
                state.packed_location = value.location;
            } else if (name == "(easypb.cpp).type") {
                if (state.has_cpp_type) {
                    throw ParseFailure(option_location,
                        "duplicate EasyProtoBuf C++ type option");
                }
                if (value.kind != Constant::STRING_VALUE) {
                    throw ParseFailure(value.location,
                        "EasyProtoBuf C++ type option must be a string literal");
                }
                if (value.text.empty()) {
                    throw ParseFailure(value.location,
                        "EasyProtoBuf C++ type must not be empty");
                }
                state.has_cpp_type = true;
                state.cpp_type = value.text;
            } else if (name == "(easypb.cpp)") {
                throw ParseFailure(option_location,
                    "use (easypb.cpp).type = \"...\" instead of assigning "
                    "(easypb.cpp) directly");
            } else if (name.compare(0, 13, "(easypb.cpp).") == 0) {
                throw ParseFailure(option_location,
                    "unknown EasyProtoBuf C++ field option " + name);
            }
        } while (accept_symbol(','));
        expect_symbol(']');
        return state;
    }

    // FieldNumber <- IntegerToken, plus protobuf field-number limits.
    std::int32_t positive_field_number()
    {
        if (current_.kind != TOKEN_INTEGER) fail("expected positive field number");
        const SourceLocation where = current_.location;
        std::uint64_t number = 0;
        if (!easypb_schema::parse_unsigned_integer(current_.text, number) || number == 0 || number > 536870911ull) {
            throw ParseFailure(where, "field number must be in range 1..536870911");
        }
        if (number >= 19000ull && number <= 19999ull) {
            throw ParseFailure(where, "field numbers 19000..19999 are reserved by Protocol Buffers");
        }
        advance();
        return static_cast<std::int32_t>(number);
    }

    // FieldRangeNumber <- IntegerToken.  Unlike an actual field
    // number, a range endpoint may include the protoc-reserved 19000..19999.
    std::int32_t field_range_number()
    {
        if (current_.kind != TOKEN_INTEGER) fail("expected positive field number");
        const SourceLocation where = current_.location;
        std::uint64_t number = 0;
        if (!easypb_schema::parse_unsigned_integer(current_.text, number) || number == 0 || number > 536870911ull) {
            throw ParseFailure(where, "field number must be in range 1..536870911");
        }
        advance();
        return static_cast<std::int32_t>(number);
    }

    // SignedInteger <- Sign? IntegerToken, constrained to int32.
    std::int32_t signed_enum_number()
    {
        std::string sign;
        const SourceLocation where = current_.location;
        if (current_.kind == TOKEN_SYMBOL && (current_.symbol == '+' || current_.symbol == '-')) {
            sign.assign(1, current_.symbol);
            advance();
        }
        if (current_.kind != TOKEN_INTEGER) fail("expected enum integer value");
        const std::string text = sign + current_.text;
        std::int32_t number = 0;
        if (!easypb_schema::parse_signed_32(text, number)) throw ParseFailure(where, "enum value does not fit int32");
        advance();
        return number;
    }

    // Converts the TypeName parsed by parse_field()/parse_map() into
    // an immediate scalar type or a deferred custom type_name. In deferred
    // mode a named type is left unresolved (has_type == false) with its raw
    // source spelling; standalone mode keeps the historical TYPE_MESSAGE
    // compatibility placeholder until the shared resolution pass runs.
    void set_type(FieldDescriptorProto& field, const std::string& type_name)
    {
        const int scalar = easypb_schema::builtin_type(type_name);
        if (scalar != 0) {
            field.type = scalar;
            field.has_type = true;
        } else if (defer_type_resolution_) {
            field.type = 0;
            field.has_type = false;
            field.type_name = out_.strings.save(type_name);
            field.has_type_name = true;
        } else {
            field.type = FieldDescriptorProto::TYPE_MESSAGE;
            field.has_type = true;
            field.type_name = out_.strings.save(type_name);
            field.has_type_name = true;
        }
    }

    // Stores FieldOption "default" using descriptor.proto conventions:
    // decoded text for string, C-escaped text for bytes, source spelling for
    // numeric/bool values, and an identifier for enum defaults.
    void apply_default(FieldDescriptorProto& field, const Constant& value)
    {
        if ((out_.file.has_syntax && view_text(out_.file.syntax) == "proto3")) {
            throw ParseFailure(value.location, "explicit default values are not allowed in proto3");
        }
        if (field.label == FieldDescriptorProto::LABEL_REPEATED || field.has_oneof_index) {
            throw ParseFailure(value.location, "default value is not allowed on repeated or oneof fields");
        }

        std::string stored;
        if (field.type == FieldDescriptorProto::TYPE_STRING) {
            if (value.kind != Constant::STRING_VALUE) {
                throw ParseFailure(value.location, "string default must be a string literal");
            }
            stored = value.text;
        } else if (field.type == FieldDescriptorProto::TYPE_BYTES) {
            if (value.kind != Constant::STRING_VALUE) {
                throw ParseFailure(value.location, "bytes default must be a string literal");
            }
            stored = escape_bytes(value.text);
        } else if (field.type == FieldDescriptorProto::TYPE_BOOL) {
            if (value.kind != Constant::BOOL_VALUE) {
                throw ParseFailure(value.location, "bool default must be true or false");
            }
            stored = value.text;
        } else if (field.type == FieldDescriptorProto::TYPE_FLOAT ||
                   field.type == FieldDescriptorProto::TYPE_DOUBLE) {
            if (value.kind != Constant::INTEGER_VALUE && value.kind != Constant::FLOAT_VALUE) {
                throw ParseFailure(value.location, "floating-point default must be numeric, inf, or nan");
            }
            if (!easypb_schema::is_float_text(value.text)) {
                throw ParseFailure(value.location, "invalid floating-point default");
            }
            stored = canonical_float_default(
                value.text, field.type == FieldDescriptorProto::TYPE_FLOAT);
        } else if (field.has_type_name) {
            if (value.kind != Constant::IDENTIFIER_VALUE) {
                throw ParseFailure(value.location, "enum default must be an identifier");
            }
            stored = value.text;
        } else {
            if (value.kind != Constant::INTEGER_VALUE) {
                throw ParseFailure(value.location, "integral default must be an integer literal");
            }
            stored = value.text;
        }
        field.default_value = out_.strings.save(stored);
        field.has_default_value = true;
    }

    // Records one parsed field's source provenance under its descriptor
    // path. The caller invokes this right after appending the field, when
    // the field index is known.
    void record_field_source(const DescriptorPath& message_path,
                             std::size_t field_index,
                             const FieldProvenance& provenance)
    {
        FieldSource source;
        source.path = easypb_schema::message_field_path(
            message_path, static_cast<int>(field_index));
        source.raw_type_name = provenance.raw_type_name;
        source.type_location = provenance.type_location;
        if (provenance.has_default) source.default_location = provenance.default_location;
        else source.default_location = easypb_schema::unknown_location();
        if (provenance.has_packed) source.packed_location = provenance.packed_location;
        else source.packed_location = easypb_schema::unknown_location();
        out_.field_sources.push_back(source);
        out_.declaration_locations[source.path] = provenance.name_location;
    }

    // Parses one field production, appends it to message.field, and records
    // its source provenance.
    void push_field(DescriptorProto& message, const DescriptorPath& message_path,
                    bool oneof_field, std::int32_t oneof_index)
    {
        FieldProvenance provenance;
        message.field.push_back(parse_field(oneof_field, oneof_index, &provenance));
        record_field_source(message_path, message.field.size() - 1, provenance);
    }

    // Field      <- Label? TypeName Identifier "=" FieldNumber
    //               FieldOptions? ";"
    // OneofField <- TypeName Identifier "=" FieldNumber FieldOptions? ";"
    // oneof_field selects the second production and records oneof_index.
    // Provenance (raw type spelling and option locations) is reported through
    // the out-parameter; extend declarations pass null because their fields
    // are validated but not retained.
    FieldDescriptorProto parse_field(bool oneof_field, std::int32_t oneof_index,
                                     FieldProvenance* provenance)
    {
        FieldDescriptorProto field;

        // Label? (absent for OneofField and permitted to be absent in proto3).
        bool label_seen = false;
        if (is_keyword("optional") || is_keyword("required") || is_keyword("repeated")) {
            if (oneof_field) fail("oneof fields must not have optional, required, or repeated labels");
            label_seen = true;
            if (accept_keyword("optional")) field.label = FieldDescriptorProto::LABEL_OPTIONAL;
            else if (accept_keyword("required")) field.label = FieldDescriptorProto::LABEL_REQUIRED;
            else {
                accept_keyword("repeated");
                field.label = FieldDescriptorProto::LABEL_REPEATED;
            }
        } else {
            field.label = FieldDescriptorProto::LABEL_OPTIONAL;
        }
        field.has_label = true;

        if ((!out_.file.has_syntax || view_text(out_.file.syntax) == "proto2") && !oneof_field && !label_seen) {
            fail("proto2 fields require optional, required, or repeated label");
        }
        if ((out_.file.has_syntax && view_text(out_.file.syntax) == "proto3") && field.label == FieldDescriptorProto::LABEL_REQUIRED) {
            fail("required fields are not allowed in proto3");
        }

        // TypeName Identifier "=" FieldNumber FieldOptions? ";"
        const SourceLocation type_location = current_.location;
        const std::string type_name = full_identifier(true);
        if (type_name == "group") fail("group fields are not supported");
        set_type(field, type_name);

        const SourceLocation name_location = current_.location;
        const std::string name = identifier();
        field.name = out_.strings.save(name);
        field.has_name = true;
        expect_symbol('=');
        field.number = positive_field_number();
        field.has_number = true;

        const FieldOptionState options = field_options();
        expect_symbol(';');

        if (oneof_field) {
            field.oneof_index = oneof_index;
            field.has_oneof_index = true;
        }
        if (options.has_default) apply_default(field, options.default_value);
        if (options.has_packed) {
            field.options.packed = options.packed;
            field.options.has_packed = true;
            field.has_options = true;
        }
        if (options.has_cpp_type) {
            field.options.cpp.type = out_.strings.save(options.cpp_type);
            field.options.cpp.has_type = true;
            field.options.has_cpp = true;
            field.has_options = true;
        }
        if (provenance != 0) {
            provenance->raw_type_name = type_name;
            provenance->type_location = type_location;
            provenance->name_location = name_location;
            provenance->has_default = options.has_default;
            if (options.has_default) {
                provenance->default_location = options.default_value.location;
            }
            provenance->has_packed = options.has_packed;
            if (options.has_packed) {
                provenance->packed_location = options.packed_location;
            }
        }
        return field;
    }

    // MapField <- "map" "<" MapKeyType "," TypeName ">"
    //             Identifier "=" FieldNumber FieldOptions? ";"
    // parse_message() has already consumed the context-sensitive "map" token.
    // This method expands the source field into protoc-compatible descriptors:
    // a synthetic nested XxxEntry message and a repeated message field. The
    // synthetic entry value keeps the source location of the map's value-type
    // token while carrying its synthetic descriptor path.
    void parse_map(DescriptorProto& message, const DescriptorPath& message_path,
                   const SourceLocation& map_location)
    {
        expect_symbol('<');
        const SourceLocation key_location = current_.location;
        const std::string key_type_name = full_identifier(true);
        const int key_type = easypb_schema::builtin_type(key_type_name);
        if (!easypb_schema::valid_map_key_type(key_type)) {
            throw ParseFailure(key_location, "invalid map key type");
        }
        expect_symbol(',');
        const SourceLocation value_location = current_.location;
        const std::string value_type_name = full_identifier(true);
        expect_symbol('>');
        const SourceLocation field_name_location = current_.location;
        const std::string field_name = identifier();
        expect_symbol('=');
        const std::int32_t number = positive_field_number();
        const FieldOptionState options = field_options();
        expect_symbol(';');
        if (options.has_default || options.has_packed) {
            // Point at the offending option value, not at the key type.
            const SourceLocation option_location = options.has_default
                ? options.default_value.location
                : options.packed_location;
            throw ParseFailure(option_location, "map fields cannot have default or packed options");
        }

        // Synthesize the nested map-entry descriptor expected by codegen.
        const std::string entry_name = camel_case(field_name) + "Entry";
        const DescriptorPath entry_path = easypb_schema::message_nested_path(
            message_path, static_cast<int>(message.nested_type.size()));
        DescriptorProto entry;
        entry.name = out_.strings.save(entry_name);
        entry.has_name = true;
        entry.options.map_entry = true;
        entry.options.has_map_entry = true;
        entry.has_options = true;

        // Entry field 1: optional MapKeyType key.
        FieldDescriptorProto key;
        key.name = out_.strings.save("key");
        key.has_name = true;
        key.number = 1;
        key.has_number = true;
        key.label = FieldDescriptorProto::LABEL_OPTIONAL;
        key.has_label = true;
        set_type(key, key_type_name);
        entry.field.push_back(key);

        // Entry field 2: optional TypeName value.
        FieldDescriptorProto value;
        value.name = out_.strings.save("value");
        value.has_name = true;
        value.number = 2;
        value.has_number = true;
        value.label = FieldDescriptorProto::LABEL_OPTIONAL;
        value.has_label = true;
        set_type(value, value_type_name);
        entry.field.push_back(value);

        // Source map field: repeated synthetic-entry message.
        FieldDescriptorProto field;
        field.name = out_.strings.save(field_name);
        field.has_name = true;
        field.number = number;
        field.has_number = true;
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        field.has_label = true;
        // In deferred mode the synthetic entry reference stays unresolved
        // like any other named type even though its entry is local; the
        // loader/linker resolves it together with the imports.
        if (defer_type_resolution_) {
            field.type = 0;
            field.has_type = false;
        } else {
            field.type = FieldDescriptorProto::TYPE_MESSAGE;
            field.has_type = true;
        }
        field.type_name = out_.strings.save(entry_name);
        field.has_type_name = true;
        if (options.has_cpp_type) {
            field.options.cpp.type = out_.strings.save(options.cpp_type);
            field.options.cpp.has_type = true;
            field.options.has_cpp = true;
            field.has_options = true;
        }

        message.nested_type.push_back(entry);
        out_.declaration_locations[entry_path] = map_location;
        // The synthetic entry holds exactly two fields pushed above: key at
        // index 0, value at index 1. A plain assert is avoided on purpose:
        // it would vanish in Release builds.
        const DescriptorPath key_path =
            easypb_schema::message_field_path(entry_path, 0);
        const DescriptorPath value_path =
            easypb_schema::message_field_path(entry_path, 1);
        out_.declaration_locations[key_path] = key_location;
        out_.declaration_locations[value_path] = value_location;
        {
            FieldSource key_source;
            key_source.path = key_path;
            key_source.raw_type_name = key_type_name;
            key_source.type_location = key_location;
            key_source.default_location = easypb_schema::unknown_location();
            key_source.packed_location = easypb_schema::unknown_location();
            out_.field_sources.push_back(key_source);
        }
        {
            FieldSource value_source;
            value_source.path = value_path;
            value_source.raw_type_name = value_type_name;
            value_source.type_location = value_location;
            value_source.default_location = easypb_schema::unknown_location();
            value_source.packed_location = easypb_schema::unknown_location();
            out_.field_sources.push_back(value_source);
        }
        message.field.push_back(field);
        {
            FieldSource field_source;
            field_source.path = easypb_schema::message_field_path(
                message_path, static_cast<int>(message.field.size() - 1));
            field_source.raw_type_name = entry_name;
            field_source.type_location = map_location;
            field_source.default_location = easypb_schema::unknown_location();
            field_source.packed_location = easypb_schema::unknown_location();
            out_.field_sources.push_back(field_source);
            out_.declaration_locations[field_source.path] = field_name_location;
        }
    }

    // Message <- "message" Identifier "{" MessageElement* "}"
    // The caller has consumed "message".  This method owns MessageElement
    // dispatch and validates reserved/extension constraints before returning.
    // message_path is this message's own descriptor path; declaration order
    // defines the appended elements' indexes, so every nested declaration
    // and field records its descriptor path immediately.
    DescriptorProto parse_message(const DescriptorPath& message_path)
    {
        const std::string name = identifier();
        DescriptorProto message;
        message.name = out_.strings.save(name);
        message.has_name = true;

        std::vector<NumberRange> reserved_ranges;
        std::set<std::string> reserved_names;
        std::vector<NumberRange> extension_ranges;
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated message body");

            // MessageElement alternatives.  Keyword tokens are consumed here;
            // the called parse_* function starts at the first token after the
            // keyword.  A plain field has no leading keyword and is the fallback.
            if (accept_symbol(';')) continue;                                      // EmptyStatement
            if (accept_keyword("message")) {                                       // Message
                const SourceLocation name_location = current_.location;
                const DescriptorPath nested_path =
                    easypb_schema::message_nested_path(
                        message_path,
                        static_cast<int>(message.nested_type.size()));
                message.nested_type.push_back(parse_message(nested_path));
                out_.declaration_locations[nested_path] = name_location;
            }
            else if (accept_keyword("enum")) {                                     // Enum
                const SourceLocation name_location = current_.location;
                const DescriptorPath enum_path = easypb_schema::message_enum_path(
                    message_path, static_cast<int>(message.enum_type.size()));
                message.enum_type.push_back(parse_enum(enum_path));
                out_.declaration_locations[enum_path] = name_location;
            }
            else if (accept_keyword("oneof")) parse_oneof(message, message_path); // Oneof
            else if (is_keyword("map") && next_is_symbol('<')) {
                const SourceLocation map_location = current_.location;
                advance();
                parse_map(message, message_path, map_location);              // MapField
            }
            else if (accept_keyword("option")) parse_option_statement();                          // OptionStatement
            else if (accept_keyword("reserved")) parse_reserved(false, reserved_ranges, reserved_names); // ReservedMessage
            else if (accept_keyword("extensions")) parse_extensions(extension_ranges);            // Extensions
            else if (accept_keyword("extend")) parse_extend();                               // Extend
            else if (is_keyword("service")) fail("service declarations are not allowed inside messages");
            else push_field(message, message_path, false, 0);                                    // Field
        }
        validate_message_constraints(message, reserved_ranges, reserved_names, extension_ranges);
        return message;
    }

    // Oneof <- "oneof" Identifier "{"
    //          (EmptyStatement / OptionStatement / OneofField)* "}"
    // The caller has consumed "oneof".  Fields are appended to message.field
    // and linked to the newly appended declaration through oneof_index.
    // message_path is the owning message's descriptor path.
    void parse_oneof(DescriptorProto& message,
                     const DescriptorPath& message_path)
    {
        const SourceLocation name_location = current_.location;
        const std::string oneof_name = identifier();
        for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
            if (view_text(message.oneof_decl[i].name) == oneof_name) {
                throw ParseFailure(name_location, "duplicate oneof name " + oneof_name);
            }
        }
        OneofDescriptorProto declaration;
        declaration.name = out_.strings.save(oneof_name);
        declaration.has_name = true;
        const std::int32_t index = static_cast<std::int32_t>(message.oneof_decl.size());
        message.oneof_decl.push_back(declaration);
        out_.declaration_locations[easypb_schema::message_oneof_path(
            message_path, index)] = name_location;
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated oneof body");

            // Oneof body alternatives: EmptyStatement, OptionStatement, or
            // OneofField.  MapField is recognized explicitly to produce a
            // more useful diagnostic than the generic field parser would.
            if (accept_symbol(';')) continue;
            if (accept_keyword("option")) parse_option_statement();
            else if (is_keyword("map") && next_is_symbol('<')) {
                fail("map fields are not allowed in oneof");
            } else {
                push_field(message, message_path, true, index);
            }
        }
    }

    // Enum <- "enum" Identifier "{" EnumElement* "}"
    // EnumElement alternatives (empty, option, reserved, value) are dispatched
    // here; enum-specific alias and proto3-first-value rules are checked here.
    // enum_path is this enum's own descriptor path, used to record each
    // value's declaration location.
    EnumDescriptorProto parse_enum(const DescriptorPath& enum_path)
    {
        const std::string name = identifier();
        EnumDescriptorProto result;
        result.name = out_.strings.save(name);
        result.has_name = true;
        bool allow_alias = false;
        std::set<std::string> value_names;
        std::vector<std::int32_t> numbers;
        std::vector<NumberRange> reserved_ranges;
        std::set<std::string> reserved_names;

        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated enum body");

            // EnumElement <- EmptyStatement / EnumOption / ReservedEnum /
            //                EnumValue
            if (accept_symbol(';')) continue;                         // EmptyStatement
            if (accept_keyword("option")) {                           // EnumOption
                const std::string option = option_name();
                expect_symbol('=');
                const Constant value = constant();
                expect_symbol(';');
                if (option == "allow_alias") {
                    if (value.kind != Constant::BOOL_VALUE) {
                        throw ParseFailure(value.location, "allow_alias must be true or false");
                    }
                    allow_alias = value.text == "true";
                }
                continue;
            }
            if (accept_keyword("reserved")) {                       // ReservedEnum
                parse_reserved(true, reserved_ranges, reserved_names);
                continue;
            }

            // Remaining alternative: EnumValue.
            const SourceLocation value_location = current_.location;
            const std::string value_name = identifier();
            if (!value_names.insert(value_name).second) {
                throw ParseFailure(value_location, "duplicate enum value name " + value_name);
            }
            expect_symbol('=');
            EnumValueDescriptorProto value;
            value.name = out_.strings.save(value_name);
            value.has_name = true;
            value.number = signed_enum_number();
            value.has_number = true;
            numbers.push_back(value.number);
            if (reserved_names.find(value_name) != reserved_names.end()) {
                throw ParseFailure(value_location, "enum value name is reserved: " + value_name);
            }
            for (std::size_t r = 0; r < reserved_ranges.size(); ++r) {
                if (range_contains(reserved_ranges[r], value.number)) {
                    throw ParseFailure(value_location, "enum value number is reserved");
                }
            }
            if (current_.kind == TOKEN_SYMBOL && current_.symbol == '[') {
                const FieldOptionState ignored = field_options();
                if (ignored.has_default || ignored.has_packed || ignored.has_cpp_type) {
                    throw ParseFailure(value_location,
                        "field-only options are not enum-value options");
                }
            }
            expect_symbol(';');
            out_.declaration_locations[easypb_schema::enum_value_path(
                enum_path, static_cast<int>(result.value.size()))] = value_location;
            result.value.push_back(value);
        }

        if (result.value.empty()) {
            throw ParseFailure(current_.location,
                               "enum must contain at least one value");
        }

        for (std::size_t i = 0; i < result.value.size(); ++i) {
            const std::string value_name = view_text(result.value[i].name);
            if (reserved_names.find(value_name) != reserved_names.end()) {
                fail("enum value name is reserved: " + value_name);
            }
            for (std::size_t r = 0; r < reserved_ranges.size(); ++r) {
                if (range_contains(reserved_ranges[r], result.value[i].number)) {
                    fail("enum value number is reserved");
                }
            }
        }

        if ((out_.file.has_syntax && view_text(out_.file.syntax) == "proto3") && !result.value.empty() && result.value[0].number != 0) {
            throw ParseFailure(current_.location, "the first proto3 enum value must be zero");
        }
        if (!allow_alias) {
            std::set<std::int32_t> unique;
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                if (!unique.insert(numbers[i]).second) {
                    throw ParseFailure(current_.location,
                        "duplicate enum number requires option allow_alias = true");
                }
            }
        }
        return result;
    }

    // Shared semantic action for MessageRange and EnumRange lists:
    // rejects reversed or overlapping ranges before recording the new range.
    void add_range(std::vector<NumberRange>& ranges, const NumberRange& range,
                   const char* kind)
    {
        if (range.last < range.first) {
            throw ParseFailure(range.location, std::string(kind) + " range end is smaller than its start");
        }
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if (ranges_overlap(ranges[i], range)) {
                throw ParseFailure(range.location, std::string(kind) + " ranges overlap");
            }
        }
        ranges.push_back(range);
    }

    // ReservedMessage <- "reserved" (MessageRanges / ReservedNames) ";"
    // ReservedEnum    <- "reserved" (EnumRanges / ReservedNames) ";"
    // The caller has consumed "reserved"; enum_context selects the range form.
    void parse_reserved(bool enum_context,
                        std::vector<NumberRange>& ranges,
                        std::set<std::string>& names)
    {
        if (current_.kind == TOKEN_STRING) {
            for (;;) {
                const SourceLocation where = current_.location;
                const std::string name = string_sequence();
                if (!names.insert(name).second) {
                    throw ParseFailure(where, "duplicate reserved name " + name);
                }
                if (!accept_symbol(',')) break;
            }
            expect_symbol(';');
            return;
        }

        for (;;) {
            NumberRange range;
            range.location = current_.location;
            range.first = enum_context ? signed_enum_number() : field_range_number();
            range.last = range.first;
            if (accept_keyword("to")) {
                if (accept_keyword("max")) {
                    range.last = enum_context ? static_cast<std::int64_t>(INT32_MAX)
                                              : static_cast<std::int64_t>(536870911);
                } else {
                    range.last = enum_context ? signed_enum_number() : field_range_number();
                }
            }
            add_range(ranges, range, "reserved");
            if (!accept_symbol(',')) break;
        }
        expect_symbol(';');
    }

    // Extensions <- "extensions" MessageRanges FieldOptions? ";"
    // The caller has consumed "extensions".  Ranges are retained temporarily
    // for conflict checks because the trimmed descriptor has no range field.
    void parse_extensions(std::vector<NumberRange>& ranges)
    {
        if ((out_.file.has_syntax && view_text(out_.file.syntax) != "proto2")) fail("extensions ranges are allowed only in proto2");
        for (;;) {
            NumberRange range;
            range.location = current_.location;
            range.first = field_range_number();
            range.last = range.first;
            if (accept_keyword("to")) {
                range.last = accept_keyword("max") ? static_cast<std::int64_t>(536870911)
                                                   : field_range_number();
            }
            add_range(ranges, range, "extension");
            if (!accept_symbol(',')) break;
        }
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '[') (void)field_options();
        expect_symbol(';');
    }

    // Post-action for Message: checks relationships among parsed fields,
    // ReservedMessage declarations, and Extensions declarations.
    void validate_message_constraints(const DescriptorProto& message,
                                      const std::vector<NumberRange>& reserved_ranges,
                                      const std::set<std::string>& reserved_names,
                                      const std::vector<NumberRange>& extension_ranges)
    {
        for (std::size_t i = 0; i < reserved_ranges.size(); ++i) {
            for (std::size_t j = 0; j < extension_ranges.size(); ++j) {
                if (ranges_overlap(reserved_ranges[i], extension_ranges[j])) {
                    throw ParseFailure(extension_ranges[j].location,
                        "reserved and extension ranges overlap");
                }
            }
        }

        for (std::size_t i = 0; i < message.field.size(); ++i) {
            const FieldDescriptorProto& field = message.field[i];
            const std::string name = view_text(field.name);
            if (reserved_names.find(name) != reserved_names.end()) {
                fail("field name is reserved: " + name);
            }
            for (std::size_t j = 0; j < reserved_ranges.size(); ++j) {
                if (range_contains(reserved_ranges[j], field.number)) {
                    fail("field number is reserved");
                }
            }
            for (std::size_t j = 0; j < extension_ranges.size(); ++j) {
                if (range_contains(extension_ranges[j], field.number)) {
                    fail("field number lies inside an extension range");
                }
            }
        }
    }

    // Extend <- "extend" TypeName "{" (EmptyStatement / Field)* "}"
    // The syntax is validated, but fields are not retained because the trimmed
    // FileDescriptorProto has no extension collection.
    void parse_extend()
    {
        const SourceLocation where = current_.location;
        if ((out_.file.has_syntax && view_text(out_.file.syntax) != "proto2")) fail("extend declarations are allowed only in proto2");
        (void)full_identifier(true);
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated extend body");
            if (accept_symbol(';')) continue;
            (void)parse_field(false, 0, 0);
        }
        Diagnostic warning;
        warning.file = file_name_;
        warning.location = where;
        warning.warning = true;
        warning.message = "extend declaration parsed but not stored by the trimmed FileDescriptorProto model";
        out_.warnings.push_back(warning);
    }

    // Semantic driver after ProtoFile has been consumed. Both modes index
    // the file with the shared easypb_schema::index_file (scope validation
    // plus symbol table). Standalone mode then resolves against local
    // declarations with easypb_schema::resolve_file, keeping the unresolved
    // warnings. Deferred mode instead runs easypb_schema::validate_deferred_file,
    // which applies only checks that need no type resolution: a locally
    // resolvable kind must not decide packed/default validity, because
    // shadowing imports may change the resolved type after linking. Named
    // types therefore stay raw with has_type == false, silently.
    void run_semantic_passes()
    {
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic semantic_error;
        if (!easypb_schema::index_file(out_, symbols, semantic_error)) {
            throw ParseFailure(semantic_error.location, semantic_error.message);
        }
        if (defer_type_resolution_) {
            if (!easypb_schema::validate_deferred_file(out_, semantic_error)) {
                throw ParseFailure(semantic_error.location, semantic_error.message);
            }
            return;
        }
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&out_);
        if (!easypb_schema::resolve_file(out_, symbols, visible, true,
                                         semantic_error)) {
            throw ParseFailure(semantic_error.location, semantic_error.message);
        }
    }
};


} // namespace

bool parse_proto(const std::string& file_name,
                 const char* source,
                 std::size_t source_size,
                 ParsedProto& result,
                 Diagnostic& error,
                 const ParseOptions& options)
{
    result.clear();
    error = Diagnostic();
    error.file = file_name;
    if (source == 0 && source_size != 0) {
        error.message = "null source buffer with non-zero size";
        return false;
    }
    try {
        Parser parser(file_name, source, source_size, result, options);
        parser.parse();
        return true;
    } catch (const ParseFailure& failure) {
        error.location = failure.location;
        error.message = failure.what();
        error.warning = false;
        return false;
    } catch (const std::exception& failure) {
        error.message = failure.what();
        error.warning = false;
        return false;
    }
}

bool parse_proto(const std::string& file_name,
                 const char* source,
                 std::size_t source_size,
                 ParsedProto& result,
                 Diagnostic& error)
{
    const ParseOptions options;
    return parse_proto(file_name, source, source_size, result, error, options);
}


} // namespace easypb_proto
