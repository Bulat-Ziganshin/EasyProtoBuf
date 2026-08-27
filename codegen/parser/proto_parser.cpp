#include "proto_parser.hpp"

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
 *   symbol collection and lexical name lookup    -> collect_message_symbols(),
 *                                                    resolve_name()
 *   type-dependent default and packed checks     -> validate_default(),
 *                                                    resolve_message()
 *   complete post-parse descriptor fix-up        -> semantic_pass()
 *
 * The parser intentionally constructs FileDescriptorProto directly rather
 * than building an intermediate AST.  Each parse_* method consumes one
 * grammar production from current_ and leaves current_ at the first token
 * following that production.
 */

namespace easypb_proto {

namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

} // namespace

StringPool::StringPool() {}

StringPool::~StringPool()
{
    clear();
}

void StringPool::clear()
{
    for (std::size_t i = 0; i < blocks_.size(); ++i) delete[] blocks_[i];
    blocks_.clear();
    capacities_.clear();
    used_.clear();
}

str_view StringPool::save(const std::string& value)
{
    return save(value.data(), value.size());
}

str_view StringPool::save(const char* data, std::size_t size)
{
    const std::size_t need = size + 1;
    if (blocks_.empty() || capacities_.back() - used_.back() < need) {
        std::size_t capacity = blocks_.empty() ? 4096u : capacities_.back() * 2u;
        if (capacity > 1024u * 1024u) capacity = 1024u * 1024u;
        if (capacity < need) capacity = need;
        blocks_.push_back(new char[capacity]);
        capacities_.push_back(capacity);
        used_.push_back(0);
    }

    char* destination = blocks_.back() + used_.back();
    if (size != 0) std::memcpy(destination, data, size);
    destination[size] = '\0';
    used_.back() += need;
    return str_view(destination, size);
}

ParsedProto::ParsedProto() {}

void ParsedProto::clear()
{
    file = FileDescriptorProto();
    imports.clear();
    warnings.clear();
    strings.clear();
}

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

bool parse_unsigned_integer(const std::string& text, std::uint64_t& result)
{
    if (text.empty()) return false;
    unsigned base = 10;
    std::size_t position = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        position = 2;
    } else if (text.size() > 1 && text[0] == '0') {
        base = 8;
        position = 1;
    }

    result = 0;
    if (position == text.size()) return true;
    for (; position < text.size(); ++position) {
        const char c = text[position];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        if (digit >= base) return false;
        if (result > (UINT64_MAX - digit) / base) return false;
        result = result * base + digit;
    }
    return true;
}

bool parse_signed_32(const std::string& text, std::int32_t& result)
{
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }
    if (start == text.size()) return false;
    std::uint64_t magnitude = 0;
    if (!parse_unsigned_integer(text.substr(start), magnitude)) return false;
    const std::uint64_t limit = negative ? 2147483648ull : 2147483647ull;
    if (magnitude > limit) return false;
    if (negative && magnitude == 2147483648ull) result = INT32_MIN;
    else result = negative ? -static_cast<std::int32_t>(magnitude)
                           : static_cast<std::int32_t>(magnitude);
    return true;
}

bool parse_signed_64(const std::string& text, std::int64_t& result)
{
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }
    if (start == text.size()) return false;
    std::uint64_t magnitude = 0;
    if (!parse_unsigned_integer(text.substr(start), magnitude)) return false;
    const std::uint64_t negative_limit = (static_cast<std::uint64_t>(INT64_MAX) + 1u);
    const std::uint64_t limit = negative ? negative_limit : static_cast<std::uint64_t>(INT64_MAX);
    if (magnitude > limit) return false;
    if (negative && magnitude == negative_limit) result = INT64_MIN;
    else result = negative ? -static_cast<std::int64_t>(magnitude)
                           : static_cast<std::int64_t>(magnitude);
    return true;
}

bool parse_unsigned_with_optional_plus(const std::string& text, std::uint64_t maximum)
{
    std::size_t start = 0;
    if (!text.empty() && text[0] == '+') start = 1;
    if (start == text.size() || (!text.empty() && text[0] == '-')) return false;
    std::uint64_t value = 0;
    return parse_unsigned_integer(text.substr(start), value) && value <= maximum;
}

bool is_float_text(const std::string& text)
{
    std::size_t start = 0;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) start = 1;
    const std::string body = text.substr(start);
    if (body == "inf" || body == "nan") return true;
    if (body.empty()) return false;
    char* end = 0;
    errno = 0;
    (void)std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && errno != ERANGE;
}

// FileDescriptorProto stores floating defaults in the same canonical form as
// protobuf's SimpleFtoa/SimpleDtoa: first try a non-over-precise decimal
// representation, then fall back to enough digits for exact round-tripping.
template <typename T>
std::string format_general(T value, int precision)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(precision) << value;
    return output.str();
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
        std::string result = format_general(value, std::numeric_limits<float>::digits10);
        char* parsed_end = 0;
        const float round_trip = static_cast<float>(std::strtod(result.c_str(), &parsed_end));
        if (parsed_end == result.c_str() || *parsed_end != '\0' || round_trip != value) {
            result = format_general(value, std::numeric_limits<float>::digits10 + 3);
        }
        return result;
    }

    std::string result = format_general(parsed, std::numeric_limits<double>::digits10);
    char* parsed_end = 0;
    const double round_trip = std::strtod(result.c_str(), &parsed_end);
    if (parsed_end == result.c_str() || *parsed_end != '\0' || round_trip != parsed) {
        result = format_general(parsed, std::numeric_limits<double>::digits10 + 2);
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

int builtin_type(const std::string& name)
{
    if (name == "double") return FieldDescriptorProto::TYPE_DOUBLE;
    if (name == "float") return FieldDescriptorProto::TYPE_FLOAT;
    if (name == "int64") return FieldDescriptorProto::TYPE_INT64;
    if (name == "uint64") return FieldDescriptorProto::TYPE_UINT64;
    if (name == "int32") return FieldDescriptorProto::TYPE_INT32;
    if (name == "fixed64") return FieldDescriptorProto::TYPE_FIXED64;
    if (name == "fixed32") return FieldDescriptorProto::TYPE_FIXED32;
    if (name == "bool") return FieldDescriptorProto::TYPE_BOOL;
    if (name == "string") return FieldDescriptorProto::TYPE_STRING;
    if (name == "bytes") return FieldDescriptorProto::TYPE_BYTES;
    if (name == "uint32") return FieldDescriptorProto::TYPE_UINT32;
    if (name == "sfixed32") return FieldDescriptorProto::TYPE_SFIXED32;
    if (name == "sfixed64") return FieldDescriptorProto::TYPE_SFIXED64;
    if (name == "sint32") return FieldDescriptorProto::TYPE_SINT32;
    if (name == "sint64") return FieldDescriptorProto::TYPE_SINT64;
    return 0;
}

bool packable_type(int type)
{
    return type != FieldDescriptorProto::TYPE_STRING &&
           type != FieldDescriptorProto::TYPE_BYTES &&
           type != FieldDescriptorProto::TYPE_MESSAGE &&
           type != FieldDescriptorProto::TYPE_GROUP && type != 0;
}

bool valid_map_key_type(int type)
{
    return type == FieldDescriptorProto::TYPE_INT32 ||
           type == FieldDescriptorProto::TYPE_INT64 ||
           type == FieldDescriptorProto::TYPE_UINT32 ||
           type == FieldDescriptorProto::TYPE_UINT64 ||
           type == FieldDescriptorProto::TYPE_SINT32 ||
           type == FieldDescriptorProto::TYPE_SINT64 ||
           type == FieldDescriptorProto::TYPE_FIXED32 ||
           type == FieldDescriptorProto::TYPE_FIXED64 ||
           type == FieldDescriptorProto::TYPE_SFIXED32 ||
           type == FieldDescriptorProto::TYPE_SFIXED64 ||
           type == FieldDescriptorProto::TYPE_BOOL ||
           type == FieldDescriptorProto::TYPE_STRING;
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

    FieldOptionState() : has_default(false), has_packed(false), packed(false) {}
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
           ParsedProto& result)
        : file_name_(file_name), lexer_(source, source_size), out_(result),
          seen_syntax_(false), seen_package_(false), seen_statement_(false)
    {
        current_ = lexer_.next();
        next_ = lexer_.next();
    }

    // ProtoFile <- EmptyStatement* SyntaxStatement? TopLevel* !.
    // Dispatches TopLevel alternatives and then runs semantic_pass().
    void parse()
    {
        out_.file.name = out_.strings.save(file_name_);
        out_.file.has_name = true;

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
            else if (accept_keyword("message")) out_.file.message_type.push_back(parse_message(std::vector<std::string>()));
            else if (accept_keyword("enum")) out_.file.enum_type.push_back(parse_enum(std::vector<std::string>()));
            else if (accept_keyword("extend")) parse_extend(std::vector<std::string>());
            else if (accept_keyword("service")) parse_service();
            else fail("expected a top-level .proto statement");
        }

        semantic_pass();
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
    std::vector<std::string> service_names_;

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
        const std::string name = full_identifier(false);
        expect_symbol(';');
        out_.file.package = out_.strings.save(name);
        out_.file.has_package = true;
        seen_package_ = true;
    }

    // Import <- "import" ("public" / "weak")? StringSequence ";"
    // Imports are retained in ParsedProto::imports; files are not loaded here.
    void parse_import()
    {
        ImportInfo info;
        info.location = current_.location;
        if (accept_keyword("public")) info.modifier = ImportInfo::PUBLIC_IMPORT;
        else if (accept_keyword("weak")) info.modifier = ImportInfo::WEAK_IMPORT;
        info.path = string_sequence();
        expect_symbol(';');
        out_.imports.push_back(info);
    }

    // OptionNamePart <- Identifier / "(" "."? FullIdentifier ")"
    std::string option_name_part()
    {
        if (!accept_symbol('(')) return identifier();

        std::string part = "(" + full_identifier(true);
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
    // Services are accepted so message schemas can be consumed by Codegen,
    // but only their names survive until package-scope validation because
    // service descriptors are outside EasyProtoBuf's trimmed model.
    void parse_service()
    {
        service_names_.push_back(identifier());
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
    // Only default and packed affect the trimmed descriptor; other options are
    // still parsed as OptionName "=" Constant and then ignored.
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
        if (!parse_unsigned_integer(current_.text, number) || number == 0 || number > 536870911ull) {
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
        if (!parse_unsigned_integer(current_.text, number) || number == 0 || number > 536870911ull) {
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
        if (!parse_signed_32(text, number)) throw ParseFailure(where, "enum value does not fit int32");
        advance();
        return number;
    }

    // Converts the TypeName parsed by parse_field()/parse_map() into
    // an immediate scalar type or a deferred custom type_name.
    void set_type(FieldDescriptorProto& field, const std::string& type_name)
    {
        const int scalar = builtin_type(type_name);
        if (scalar != 0) {
            field.type = scalar;
            field.has_type = true;
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
            if (!is_float_text(value.text)) {
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

    // Field      <- Label? TypeName Identifier "=" FieldNumber
    //               FieldOptions? ";"
    // OneofField <- TypeName Identifier "=" FieldNumber FieldOptions? ";"
    // oneof_field selects the second production and records oneof_index.
    FieldDescriptorProto parse_field(bool oneof_field, std::int32_t oneof_index)
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
        const std::string type_name = full_identifier(true);
        if (type_name == "group") fail("group fields are not supported");
        set_type(field, type_name);

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
        return field;
    }

    // MapField <- "map" "<" MapKeyType "," TypeName ">"
    //             Identifier "=" FieldNumber FieldOptions? ";"
    // parse_message() has already consumed the context-sensitive "map" token.
    // This method expands the source field into protoc-compatible descriptors:
    // a synthetic nested XxxEntry message and a repeated message field.
    void parse_map(DescriptorProto& message)
    {
        expect_symbol('<');
        const SourceLocation key_location = current_.location;
        const std::string key_type_name = full_identifier(true);
        const int key_type = builtin_type(key_type_name);
        if (!valid_map_key_type(key_type)) {
            throw ParseFailure(key_location, "invalid map key type");
        }
        expect_symbol(',');
        const std::string value_type_name = full_identifier(true);
        expect_symbol('>');
        const std::string field_name = identifier();
        expect_symbol('=');
        const std::int32_t number = positive_field_number();
        const FieldOptionState options = field_options();
        expect_symbol(';');
        if (options.has_default || options.has_packed) {
            throw ParseFailure(key_location, "map fields cannot have default or packed options");
        }

        // Synthesize the nested map-entry descriptor expected by codegen.
        const std::string entry_name = camel_case(field_name) + "Entry";
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
        field.type = FieldDescriptorProto::TYPE_MESSAGE;
        field.has_type = true;
        field.type_name = out_.strings.save(entry_name);
        field.has_type_name = true;

        message.nested_type.push_back(entry);
        message.field.push_back(field);
    }

    // Message <- "message" Identifier "{" MessageElement* "}"
    // The caller has consumed "message".  This method owns MessageElement
    // dispatch and validates reserved/extension constraints before returning.
    DescriptorProto parse_message(const std::vector<std::string>& parent_scope)
    {
        const std::string name = identifier();
        DescriptorProto message;
        message.name = out_.strings.save(name);
        message.has_name = true;

        std::vector<std::string> scope = parent_scope;
        scope.push_back(name);
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
            if (accept_keyword("message")) message.nested_type.push_back(parse_message(scope)); // Message

            else if (accept_keyword("enum")) message.enum_type.push_back(parse_enum(scope));     // Enum
            else if (accept_keyword("oneof")) parse_oneof(message, scope);                         // Oneof
            else if (is_keyword("map") && next_is_symbol('<')) {
                advance();
                parse_map(message);                                                        // MapField
            }
            else if (accept_keyword("option")) parse_option_statement();                          // OptionStatement
            else if (accept_keyword("reserved")) parse_reserved(false, reserved_ranges, reserved_names); // ReservedMessage
            else if (accept_keyword("extensions")) parse_extensions(extension_ranges);            // Extensions
            else if (accept_keyword("extend")) parse_extend(scope);                               // Extend
            else if (is_keyword("service")) fail("service declarations are not allowed inside messages");
            else message.field.push_back(parse_field(false, 0));                                  // Field
        }
        validate_message_constraints(message, reserved_ranges, reserved_names, extension_ranges);
        return message;
    }

    // Oneof <- "oneof" Identifier "{"
    //          (EmptyStatement / OptionStatement / OneofField)* "}"
    // The caller has consumed "oneof".  Fields are appended to message.field
    // and linked to the newly appended declaration through oneof_index.
    void parse_oneof(DescriptorProto& message, const std::vector<std::string>& scope)
    {
        (void)scope;
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
                message.field.push_back(parse_field(true, index));
            }
        }
    }

    // Enum <- "enum" Identifier "{" EnumElement* "}"
    // EnumElement alternatives (empty, option, reserved, value) are dispatched
    // here; enum-specific alias and proto3-first-value rules are checked here.
    EnumDescriptorProto parse_enum(const std::vector<std::string>& scope)
    {
        (void)scope;
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
                if (ignored.has_default || ignored.has_packed) {
                    throw ParseFailure(value_location, "default and packed are not enum-value options");
                }
            }
            expect_symbol(';');
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
    void parse_extend(const std::vector<std::string>& scope)
    {
        (void)scope;
        const SourceLocation where = current_.location;
        if ((out_.file.has_syntax && view_text(out_.file.syntax) != "proto2")) fail("extend declarations are allowed only in proto2");
        (void)full_identifier(true);
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated extend body");
            if (accept_symbol(';')) continue;
            (void)parse_field(false, 0);
        }
        Diagnostic warning;
        warning.file = file_name_;
        warning.location = where;
        warning.warning = true;
        warning.message = "extend declaration parsed but not stored by the trimmed FileDescriptorProto model";
        out_.warnings.push_back(warning);
    }

    std::string package_prefix() const
    {
        if (!out_.file.has_package || out_.file.package.empty()) return std::string();
        return "." + view_text(out_.file.package);
    }

    void add_symbol(std::map<std::string, int>& symbols, const std::string& name,
                    int kind, const SourceLocation& where)
    {
        if (!symbols.insert(std::make_pair(name, kind)).second) {
            throw ParseFailure(where, "duplicate type name " + name);
        }
    }

    // Add one declaration to a protobuf lexical scope.  Enum values use their
    // containing scope rather than the enum type's scope, so all declaration
    // categories must share this table.
    void add_scope_name(std::map<std::string, std::string>& names,
                        const std::string& name,
                        const std::string& kind,
                        const std::string& scope)
    {
        const std::pair<std::map<std::string, std::string>::iterator, bool> inserted =
            names.insert(std::make_pair(name, kind));
        if (!inserted.second) {
            fail("name collision in " +
                 (scope.empty() ? std::string("global scope") : scope) +
                 ": " + kind + " " + name + " conflicts with " +
                 inserted.first->second);
        }
    }

    // Validate the file/package scope shared by top-level declarations.
    void validate_file_scope_names(const std::string& scope)
    {
        std::map<std::string, std::string> names;
        for (std::size_t i = 0; i < out_.file.enum_type.size(); ++i) {
            add_scope_name(names, view_text(out_.file.enum_type[i].name),
                           "enum type", scope);
        }
        for (std::size_t i = 0; i < out_.file.message_type.size(); ++i) {
            add_scope_name(names, view_text(out_.file.message_type[i].name),
                           "message", scope);
        }
        for (std::size_t i = 0; i < service_names_.size(); ++i) {
            add_scope_name(names, service_names_[i], "service", scope);
        }
        for (std::size_t i = 0; i < out_.file.enum_type.size(); ++i) {
            for (std::size_t j = 0; j < out_.file.enum_type[i].value.size(); ++j) {
                add_scope_name(names, view_text(out_.file.enum_type[i].value[j].name),
                               "enum value", scope);
            }
        }
    }

    // Validate one message scope, then recurse into each nested message scope.
    void validate_message_scope_names(const DescriptorProto& message,
                                      const std::string& parent)
    {
        const std::string scope = parent + "." + view_text(message.name);
        std::map<std::string, std::string> names;
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            add_scope_name(names, view_text(message.field[i].name), "field", scope);
        }
        for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
            add_scope_name(names, view_text(message.nested_type[i].name),
                           "nested message", scope);
        }
        for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
            add_scope_name(names, view_text(message.enum_type[i].name),
                           "nested enum type", scope);
        }
        for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
            add_scope_name(names, view_text(message.oneof_decl[i].name),
                           "oneof", scope);
        }
        for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
            for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
                add_scope_name(names, view_text(message.enum_type[i].value[j].name),
                               "enum value", scope);
            }
        }
        for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
            validate_message_scope_names(message.nested_type[i], scope);
        }
    }

    typedef std::map<std::string, const EnumDescriptorProto*> EnumByName;

    // Recursively collects fully-qualified message and enum names for
    // the semantic type-resolution pass.
    void collect_message_symbols(const DescriptorProto& message, const std::string& parent,
                                 std::map<std::string, int>& symbols,
                                 EnumByName& enum_types)
    {
        const std::string name = parent + "." + view_text(message.name);
        SourceLocation synthetic;
        add_symbol(symbols, name, FieldDescriptorProto::TYPE_MESSAGE, synthetic);
        for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
            const std::string enum_name =
                name + "." + view_text(message.enum_type[i].name);
            add_symbol(symbols, enum_name, FieldDescriptorProto::TYPE_ENUM, synthetic);
            enum_types[enum_name] = &message.enum_type[i];
        }
        for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
            collect_message_symbols(message.nested_type[i], name, symbols, enum_types);
        }
    }

    // Implements protobuf lexical name lookup: an absolute name is
    // checked directly; a relative name is tried from the innermost scope
    // outward to the package/global scope.
    std::string resolve_name(const std::string& raw, const std::string& scope,
                             const std::map<std::string, int>& symbols) const
    {
        if (!raw.empty() && raw[0] == '.') {
            return symbols.find(raw) != symbols.end() ? raw : std::string();
        }
        std::string current_scope = scope;
        for (;;) {
            const std::string candidate = current_scope.empty() ? "." + raw : current_scope + "." + raw;
            if (symbols.find(candidate) != symbols.end()) return candidate;
            if (current_scope.empty()) break;
            const std::size_t dot = current_scope.rfind('.');
            if (dot == std::string::npos || dot == 0) current_scope.clear();
            else current_scope.erase(dot);
        }
        return std::string();
    }

    void warning(const std::string& message,
                 DiagnosticCode code = DIAGNOSTIC_GENERIC)
    {
        Diagnostic diagnostic;
        diagnostic.file = file_name_;
        diagnostic.warning = true;
        diagnostic.message = message;
        diagnostic.code = code;
        out_.warnings.push_back(diagnostic);
    }

    // Type-dependent semantic validation for the textual default_value
    // previously stored by apply_default().
    void validate_default(const FieldDescriptorProto& field, bool unresolved,
                          const EnumByName& enum_types)
    {
        if (!field.has_default_value) return;
        const std::string value = view_text(field.default_value);
        std::int32_t signed32 = 0;
        std::int64_t signed64 = 0;
        switch (field.type) {
            case FieldDescriptorProto::TYPE_STRING:
            case FieldDescriptorProto::TYPE_BYTES:
                return;
            case FieldDescriptorProto::TYPE_BOOL:
                if (value != "true" && value != "false") fail("bool default must be true or false");
                return;
            case FieldDescriptorProto::TYPE_FLOAT:
            case FieldDescriptorProto::TYPE_DOUBLE:
                if (!is_float_text(value)) fail("invalid floating-point default");
                return;
            case FieldDescriptorProto::TYPE_ENUM:
                if (value.empty()) fail("enum default must name an enum value");
                {
                    const std::string enum_name = view_text(field.type_name);
                    const EnumByName::const_iterator found = enum_types.find(enum_name);
                    if (found == enum_types.end()) {
                        fail("cannot validate default for unknown enum type " + enum_name);
                    }
                    for (std::size_t i = 0; i < found->second->value.size(); ++i) {
                        if (view_text(found->second->value[i].name) == value) return;
                    }
                    fail("enum type " + enum_name + " has no value named " + value);
                }
                return;
            case FieldDescriptorProto::TYPE_MESSAGE:
                if (unresolved) {
                    warning("cannot validate default for unresolved imported type " + view_text(field.type_name));
                    return;
                }
                fail("message fields cannot have defaults");
                return;
            case FieldDescriptorProto::TYPE_INT32:
            case FieldDescriptorProto::TYPE_SINT32:
            case FieldDescriptorProto::TYPE_SFIXED32:
                if (!parse_signed_32(value, signed32)) fail("default does not fit signed 32-bit field");
                return;
            case FieldDescriptorProto::TYPE_UINT32:
            case FieldDescriptorProto::TYPE_FIXED32:
                if (!parse_unsigned_with_optional_plus(value, UINT32_MAX)) fail("default does not fit unsigned 32-bit field");
                return;
            case FieldDescriptorProto::TYPE_INT64:
            case FieldDescriptorProto::TYPE_SINT64:
            case FieldDescriptorProto::TYPE_SFIXED64:
                if (!parse_signed_64(value, signed64)) fail("default does not fit signed 64-bit field");
                return;
            case FieldDescriptorProto::TYPE_UINT64:
            case FieldDescriptorProto::TYPE_FIXED64:
                if (!parse_unsigned_with_optional_plus(value, UINT64_MAX)) fail("default does not fit unsigned 64-bit field");
                return;
            default:
                fail("unsupported field type in default validation");
                return;
        }
    }

    // Resolves every custom field TypeName in one Message subtree and
    // applies semantic rules that require the resolved type: packed legality,
    // oneof_index bounds, and default-value validation.
    void resolve_message(DescriptorProto& message, const std::string& parent,
                         const std::map<std::string, int>& symbols,
                         const EnumByName& enum_types)
    {
        const std::string scope = parent + "." + view_text(message.name);
        std::set<std::string> names;
        std::set<std::int32_t> numbers;
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            FieldDescriptorProto& field = message.field[i];
            if (!names.insert(view_text(field.name)).second) fail("duplicate field name " + view_text(field.name));
            if (!numbers.insert(field.number).second) fail("duplicate field number in message " + scope);

            bool unresolved = false;
            if (field.has_type_name) {
                const std::string raw = view_text(field.type_name);
                const std::string resolved = resolve_name(raw, scope, symbols);
                if (!resolved.empty()) {
                    field.type = symbols.find(resolved)->second;
                    field.has_type = true;
                    field.type_name = out_.strings.save(resolved);
                } else {
                    unresolved = true;
                    warning("unresolved type " + raw + " in " + scope +
                            "; it is kept as TYPE_MESSAGE until imports are linked",
                            DIAGNOSTIC_UNRESOLVED_TYPE);
                }
            }

            if (field.options.has_packed) {
                if (field.label != FieldDescriptorProto::LABEL_REPEATED || !packable_type(field.type)) {
                    fail("packed option is valid only on repeated primitive or enum fields");
                }
            }
            if (field.has_oneof_index &&
                (field.oneof_index < 0 ||
                 static_cast<std::size_t>(field.oneof_index) >= message.oneof_decl.size())) {
                fail("invalid oneof_index in message " + scope);
            }
            validate_default(field, unresolved, enum_types);
        }

        std::set<std::string> nested_names;
        for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
            if (!nested_names.insert(view_text(message.enum_type[i].name)).second) {
                fail("duplicate nested type name in " + scope);
            }
        }
        for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
            if (!nested_names.insert(view_text(message.nested_type[i].name)).second) {
                fail("duplicate nested type name in " + scope);
            }
            resolve_message(message.nested_type[i], scope, symbols, enum_types);
        }
    }

    // Semantic pass after ProtoFile has been consumed:
    // 1. collect all local message/enum symbols;
    // 2. resolve field type names and finish descriptor validation.
    void semantic_pass()
    {
        // Pass 1: build the complete table before resolving any field, so
        // forward references and mutually-referential messages work.
        std::map<std::string, int> symbols;
        EnumByName enum_types;
        const std::string prefix = package_prefix();
        SourceLocation synthetic;
        validate_file_scope_names(prefix);
        for (std::size_t i = 0; i < out_.file.message_type.size(); ++i) {
            validate_message_scope_names(out_.file.message_type[i], prefix);
        }
        for (std::size_t i = 0; i < out_.file.enum_type.size(); ++i) {
            const std::string enum_name =
                prefix + "." + view_text(out_.file.enum_type[i].name);
            add_symbol(symbols, enum_name, FieldDescriptorProto::TYPE_ENUM, synthetic);
            enum_types[enum_name] = &out_.file.enum_type[i];
        }
        for (std::size_t i = 0; i < out_.file.message_type.size(); ++i) {
            collect_message_symbols(out_.file.message_type[i], prefix,
                                    symbols, enum_types);
        }
        // Pass 2: resolve TypeName values and apply type-dependent checks.
        for (std::size_t i = 0; i < out_.file.message_type.size(); ++i) {
            resolve_message(out_.file.message_type[i], prefix, symbols, enum_types);
        }
    }
};


} // namespace

bool parse_proto(const std::string& file_name,
                 const char* source,
                 std::size_t source_size,
                 ParsedProto& result,
                 Diagnostic& error)
{
    result.clear();
    error = Diagnostic();
    error.file = file_name;
    if (source == 0 && source_size != 0) {
        error.message = "null source buffer with non-zero size";
        return false;
    }
    try {
        Parser parser(file_name, source, source_size, result);
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


} // namespace easypb_proto
