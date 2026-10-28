#include "proto_parser.hpp"

#include "known_options.hpp"
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
 *   proto3 `optional` presence and oneofs (D1-A)  -> synthesize_proto3_oneofs()
 *   known-option name/kind/placement (D2-A)       -> known_options.hpp/.cpp
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

// W-35: the guarded conversion lives in schema.hpp, beside the storage it
// converts; one door for every frontend.
using easypb_schema::view_text;

typedef easypb_schema::DescriptorPath DescriptorPath;
typedef easypb_schema::FieldSource FieldSource;

namespace {

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
    // Plan 5 follow-up S7e (W-60): the code the throw site already knows.
    // Every grammar refusal that has a descriptor twin names that twin's
    // code; the rest stay DIAGNOSTIC_GENERIC, which is the Diagnostic
    // default, so an unadorned throw keeps its old meaning.
    easypb_schema::DiagnosticCode code;

    ParseFailure(const SourceLocation& where, const std::string& message,
                 easypb_schema::DiagnosticCode failure_code =
                     easypb_schema::DIAGNOSTIC_GENERIC)
        : std::runtime_error(message), location(where), code(failure_code) {}
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

void append_utf8(std::string& output, unsigned long codepoint)
{
    // CR-13: mirrors AppendUTF8 (`io/tokenizer.cc:1049-1075`). A lone
    // surrogate is not an error: protoc encodes it as the three bytes its
    // numeric value spells (the string is bogus, but the author wrote it and
    // the descriptor carries the same bytes). Above 0x10FFFF protoc appends
    // the escape text back verbatim, because `ConsumeString` bounds `\U` to
    // 0x01FFFFFF and `AppendUTF8` has no fourth alternative.
    if (codepoint > 0x10fffful) {
        static const char hex_digits[] = "0123456789abcdef";
        output += "\\U";
        for (int shift = 28; shift >= 0; shift -= 4) {
            output.push_back(hex_digits[(codepoint >> shift) & 0xful]);
        }
        return;
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
        : source_(source), source_size_(source_size), offset_(0), line_(1), column_(1)
    {
        // W-41: protoc consumes a UTF-8 BOM only at the start of the file
        // (`io/tokenizer.cc:845-856`). The lexer is the one door every source
        // text passes, including an in-memory `parse_proto`, so the bytes are
        // consumed here rather than in the shared disk reader (a `.pbs` is
        // binary and must keep its first three bytes as data). `advance()`
        // three times keeps the column arithmetic aligned with protoc: a
        // following `1:10` error is reported as `1:13`.
        if (source_size_ >= 3 &&
            static_cast<unsigned char>(source_[0]) == 0xEFu &&
            static_cast<unsigned char>(source_[1]) == 0xBBu &&
            static_cast<unsigned char>(source_[2]) == 0xBFu) {
            advance();
            advance();
            advance();
        }
    }

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

        // CR-19: `std::strchr` finds its own terminator when `c` is NUL, so a
        // raw NUL byte used to look like punctuation and the aggregate-option
        // scanner swallowed it. Protoc reports every control byte here
        // ("Invalid control characters encountered in text.",
        // `io/tokenizer.cc:636-648`); the message hex-escapes it so no control
        // byte reaches a diagnostic (P9). The unsigned cast keeps a high-bit
        // byte (0x80..0xFF) on the "unexpected character" path below.
        if (static_cast<unsigned char>(c) < 0x20u) {
            std::ostringstream message;
            message << "invalid control character 0x" << std::hex << std::setw(2)
                    << std::setfill('0')
                    << static_cast<unsigned>(static_cast<unsigned char>(c))
                    << " in text";
            throw ParseFailure(token.location, message.str());
        }

        static const char* punctuation = "{}[]()<>=;,.+-:";
        if (std::strchr(punctuation, c) != 0) {
            token.kind = TOKEN_SYMBOL;
            token.symbol = c;
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

    // Consumes exactly one source byte with protoc's `Tokenizer::NextChar`
    // position rule (`io/tokenizer.cc:246-256`): only '\n' starts a line, a
    // bare CR is content, and a tab advances to the next multiple of 8. CRLF
    // still counts one line - the CR is one column of content and the LF
    // resets the column - so a CRLF file keeps the line numbering the
    // whitespace scanner had when it folded the pair. U-17 and U-33.
    void advance()
    {
        if (offset_ >= source_size_) return;
        const char c = source_[offset_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else if (c == '\t') {
            column_ += 8 - (column_ - 1) % 8;
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
                // U-17: `Tokenizer::ConsumeLineComment` stops only at LF or
                // end of input; a bare CR is ordinary comment content.
                while (peek() != '\0' && peek() != '\n') advance();
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                const SourceLocation start = location();
                advance();
                advance();
                for (;;) {
                    if (peek() == '\0') {
                        throw ParseFailure(start, "unterminated block comment");
                    }
                    // CR-30: protoc refuses a nested opener rather than
                    // scanning past it (`ConsumeBlockComment`,
                    // `io/tokenizer.cc:540-550`); the first `*/` closes the
                    // comment, so `/* /* */ */` leaves a stray `*/`.
                    if (peek() == '/' && peek(1) == '*') {
                        throw ParseFailure(location(),
                                           "\"/*\" inside a block comment: "
                                           "block comments cannot be nested");
                    }
                    if (peek() == '*' && peek(1) == '/') break;
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
    // The three branches mirror `Tokenizer::ConsumeNumber`
    // (`io/tokenizer.cc:440-495`): a leading zero makes the token octal (or
    // hex), and an octal token has neither a fraction nor an exponent.
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
            if (peek() == '.') {
                throw ParseFailure(token.location, "hex and octal numbers must be integers");
            }
            if (ascii_alpha(peek()) || ascii_digit(peek()) || peek() == '_') {
                throw ParseFailure(token.location, "invalid suffix on hexadecimal integer literal");
            }
            token.kind = TOKEN_INTEGER;
            token.text.assign(source_ + start, offset_ - start);
            return token;
        }

        // CR-18: after a leading zero, protoc reads octal digits and refuses
        // every digit it could not read, so `08` and `09` never become a
        // decimal 8 or 9. `0x` is the hex branch above and stays valid.
        if (peek() == '0' && ascii_digit(peek(1))) {
            advance();
            while (peek() >= '0' && peek() <= '7') advance();
            if (ascii_digit(peek())) {
                throw ParseFailure(token.location,
                                   "numbers starting with a leading zero must be octal");
            }
            if (peek() == '.') {
                throw ParseFailure(token.location, "hex and octal numbers must be integers");
            }
            if (ascii_alpha(peek()) || peek() == '_') {
                throw ParseFailure(token.location, "invalid suffix on numeric literal");
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

        // U-26: a decimal number carries at most one decimal point or exponent.
        // `Tokenizer::ConsumeNumber` reports this at the second dot
        // (`io/tokenizer.cc:485-492`); without the check `1.2.3` tokenized as
        // `1.2`, `.`, `3` and an aggregate body silently admitted it.
        if (peek() == '.') {
            throw ParseFailure(token.location,
                               "already saw decimal point or exponent; can't have another one");
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
    // The scan mirrors `Tokenizer::ConsumeString` + `ParseStringAppend`
    // (`io/tokenizer.cc:378-430`, `1156-1235`): a raw CR is content (CR-29)
    // and only a raw LF or NUL ends the literal with an error.
    Token string_literal()
    {
        Token token;
        token.kind = TOKEN_STRING;
        token.location = location();
        const char quote = peek();
        advance();

        while (peek() != quote) {
            if (peek() == '\0') throw ParseFailure(token.location, "unterminated string literal");
            if (peek() == '\n') {
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
                case 'x':
                case 'X': {
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
                    // `ConsumeString` bounds `\U` to `00[01]xxxxxx`, i.e. at
                    // most 0x01FFFFFF; AppendUTF8 owns everything above
                    // 0x10FFFF (see append_utf8).
                    if (digits == 8u &&
                        (peek() != '0' || peek(1) != '0' ||
                         (peek(2) != '0' && peek(2) != '1'))) {
                        throw ParseFailure(
                            escape_location,
                            "Unicode escape must be eight hex digits up to 10ffff");
                    }
                    unsigned long value = 0;
                    for (unsigned i = 0; i < digits; ++i) {
                        if (!ascii_hex(peek())) {
                            throw ParseFailure(escape_location, "incomplete Unicode escape");
                        }
                        value = value * 16ul + static_cast<unsigned long>(hex_value(peek()));
                        advance();
                    }
                    // CR-13: FetchUnicodePoint assembles a head + trail
                    // UTF-16 pair; a trail that is not a valid second unit
                    // leaves the head as a lone code point.
                    if (value >= 0xd800ul && value < 0xdc00ul &&
                        peek() == '\\' && peek(1) == 'u') {
                        unsigned long trail = 0;
                        bool complete = true;
                        for (unsigned i = 0; i < 4; ++i) {
                            if (!ascii_hex(peek(2 + i))) {
                                complete = false;
                                break;
                            }
                            trail = trail * 16ul +
                                    static_cast<unsigned long>(hex_value(peek(2 + i)));
                        }
                        if (complete && trail >= 0xdc00ul && trail < 0xe000ul) {
                            value = 0x10000ul +
                                    ((value - 0xd800ul) << 10) +
                                    (trail - 0xdc00ul);
                            for (unsigned i = 0; i < 6; ++i) advance();
                        }
                    }
                    append_utf8(token.text, value);
                    break;
                }
                default:
                    if (escaped >= '0' && escaped <= '7') {
                        // CR-12: protoc stores the low byte with no range
                        // error (`ParseStringAppend`), so `\777` is 0xFF and
                        // `\400` is 0x00.
                        unsigned value = static_cast<unsigned>(escaped - '0');
                        unsigned count = 1;
                        while (count != 3 && peek() >= '0' && peek() <= '7') {
                            value = value * 8u + static_cast<unsigned>(peek() - '0');
                            advance();
                            ++count;
                        }
                        token.text.push_back(static_cast<char>(value & 0xffu));
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

// The shared helpers parse_unsigned_integer, parse_signed_32,
// canonical_integral_default, canonical_float_default,
// integer_token_as_double and the scalar-type classifiers (builtin_type,
// packable_type, valid_map_key_type) live in schema_semantics.hpp/.cpp so the
// parser and the shared semantic passes classify types and validate scalar
// defaults identically. The 64-bit default-range helpers stay private to the
// semantic module: default range validation moved there with validate_default.

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
    bool has_json_name;
    std::string json_name;
    SourceLocation json_name_location;
    // D2-A: `lazy`/`unverified_lazy` are legal only on a message field. The
    // kind of a named type is unknown until linking, so the parser records
    // `true` and the shared field pass decides with the resolved kind; `false`
    // is the option default and never reaches the rule (U-08).
    bool has_lazy;
    SourceLocation lazy_location;
    // D2-A: the extension-range pair `declaration` + `verification` is
    // validated together after the list closes. protoc refuses `UNVERIFIED`
    // beside any declaration (descriptor.cc, `ValidateExtensionRangeOptions`).
    bool has_declaration;
    bool has_verification;
    std::string verification;
    SourceLocation verification_location;

    FieldOptionState()
        : has_default(false), has_packed(false), packed(false),
          has_cpp_type(false), has_json_name(false), has_lazy(false),
          has_declaration(false), has_verification(false) {}
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
    bool has_lazy;
    SourceLocation lazy_location;

    FieldProvenance() : has_default(false), has_packed(false), has_lazy(false) {}
};

// V-12: one proto3 `extend` statement whose allowlist verdict waits for the
// end of the file. The extendee is compared by its resolved full name, and the
// package it resolves in may be declared after the statement, so the check
// cannot run while the statement is on the cursor.
struct Proto3ExtendCheck
{
    std::string spelling;
    SourceLocation location;
};

struct NumberRange
{
    std::int64_t first;
    std::int64_t last;
    SourceLocation location;
    // V-05: an extension range written as `N to max` is expanded after the
    // message body, because its value depends on `message_set_wire_format`,
    // which may be declared after the range (protoc's
    // `AdjustExtensionRangesWithMaxEndNumber`, parser.cc:827-837). `last`
    // carries the widest value meanwhile so overlap detection never sees a
    // hole; `max_end` marks the range for the substitution.
    bool max_end;

    NumberRange() : first(0), last(0), max_end(false) {}
};

// V-05: the largest inclusive extension number a MessageSet may declare.
// protoc's parser refuses an explicit INT32_MAX endpoint ("Field number out of
// bounds.", parser.cc:1636-1640) and expands `to max` to INT32_MAX as the
// exclusive end (`:830-837`), so the last spellable number is INT32_MAX - 1.
const std::int32_t kMaxMessageSetNumber = 2147483646;

bool range_contains(const NumberRange& range, std::int64_t value)
{
    return value >= range.first && value <= range.last;
}

bool ranges_overlap(const NumberRange& left, const NumberRange& right)
{
    return left.first <= right.last && right.first <= left.last;
}

// U-16: protoc's `AllowedExtendeeInProto3` list (`descriptor.cc:1105-1131`).
// Declaration options are the one extension surface proto3 allows, and the
// reference compiler compares the *resolved* extendee's full name with these
// nine names (`ValidateProto3Field`, descriptor.cc:7907-7911) after its symbol
// lookup. V-12: the comparison therefore takes the file's package into
// account. A relative spelling is tried in the package scope and in the global
// scope, the two candidates protoc's `LookupSymbol` reaches for a file-scope
// name; a leading dot is the package separator of an already absolute name.
// An absolute spelling is compared as written.
bool allowed_proto3_extendee(const std::string& spelling,
                             const std::string& package)
{
    static const char* const kOptionMessages[] = {
        "FileOptions",
        "MessageOptions",
        "FieldOptions",
        "EnumOptions",
        "EnumValueOptions",
        "ServiceOptions",
        "MethodOptions",
        "OneofOptions",
        "ExtensionRangeOptions"
    };
    std::string candidates[2];
    std::size_t candidate_count = 0;
    const bool absolute = !spelling.empty() && spelling[0] == '.';
    if (absolute) {
        candidates[candidate_count++] = spelling.substr(1);
    } else {
        candidates[candidate_count++] = spelling;
        if (!package.empty()) {
            candidates[candidate_count++] = package + "." + spelling;
        }
    }
    for (std::size_t c = 0; c < candidate_count; ++c) {
        for (std::size_t i = 0;
             i < sizeof(kOptionMessages) / sizeof(kOptionMessages[0]); ++i) {
            if (candidates[c] ==
                std::string("google.protobuf.") + kOptionMessages[i]) {
                return true;
            }
        }
    }
    return false;
}

class Parser
{
public:
    Parser(const std::string& file_name, const char* source, std::size_t source_size,
           ParsedProto& result, const ParseOptions& options)
        : file_name_(file_name), lexer_(source, source_size), out_(result),
          seen_syntax_(false), seen_package_(false),
          defer_type_resolution_(options.defer_type_resolution),
          syntax_(easypb_schema::FILE_SYNTAX_PROTO2)
    {
        current_ = lexer_.next();
        next_ = lexer_.next();
    }

    // ProtoFile <- SyntaxStatement? TopLevel* !.
    // `syntax` is recognized only when it is the first token, as protoc's
    // `ParseProtoFile` does (`parser.cc:600-612` tests `LookingAt("syntax")`
    // before its statement loop). A leading empty statement therefore accepts
    // no following `syntax` declaration; empty statements after it stay legal.
    // Dispatches TopLevel alternatives and then runs the shared semantic
    // passes (easypb_schema::index_file for both modes, plus
    // easypb_schema::resolve_file for validation).
    void parse()
    {
        out_.file.name = out_.strings.save(file_name_);
        out_.file.has_name = true;
        out_.physical_name = file_name_;
        out_.from_descriptor_set = false;

        bool syntax_window_open = true;
        while (current_.kind != TOKEN_END) {
            // EmptyStatement. Consuming one closes the syntax window: protoc
            // only looks for `syntax` before any statement token.
            if (accept_symbol(';')) {
                syntax_window_open = false;
                continue;
            }

            // SyntaxStatement is special: unlike TopLevel, it may occur only
            // once, before every other token (comments and a BOM are skipped
            // by the lexer, so they do not close the window).
            if (is_keyword("syntax")) {
                if (!syntax_window_open || seen_syntax_) {
                    fail("syntax declaration must be the first statement");
                }
                parse_syntax();
                seen_syntax_ = true;
                syntax_window_open = false;
                continue;
            }
            syntax_window_open = false;

            // TopLevel <- Package / Import / OptionStatement / Message /
            //             Enum / Extend / Service
            if (accept_keyword("package")) parse_package();
            else if (accept_keyword("import")) parse_import();
            else if (accept_keyword("option"))
                parse_option_statement(OPTION_OWNER_FILE, file_option_names_);
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
            // EditionsStatement: `edition = "2023";` names a syntax this
            // project cannot model (A25), so it is refused by name instead of
            // falling through to the generic top-level message, which read
            // like a typo (plan 4's ledger D-44; plan 5 owns this branch).
            // The refusal is a statement-level one and does not weaken the
            // unknown-`syntax` path above, which keeps its own message.
            else if (is_keyword("edition")) fail_editions_statement();
            else fail("expected a top-level .proto statement");
        }

        validate_proto3_extends();
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
    bool defer_type_resolution_;
    easypb_schema::FileSyntax syntax_;
    // V-12: the proto3 `extend` statements of this file, validated once the
    // final package spelling is known.
    std::vector<Proto3ExtendCheck> proto3_extends_;
    // D2-A: file-scope options already seen, for protoc's
    // `Option "x" was already set.` rule. Every nested scope keeps its own
    // set beside its parse loop.
    std::set<std::string> file_option_names_;

    void advance()
    {
        current_ = std::move(next_);
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

    // The code-aware spelling for a refusal whose descriptor twin already has
    // a DiagnosticCode (W-60).
    void fail(const std::string& message,
              easypb_schema::DiagnosticCode code) const
    {
        throw ParseFailure(current_.location, message, code);
    }

    // Identifier.  The lexer already validated its spelling.
    std::string identifier()
    {
        if (current_.kind != TOKEN_IDENTIFIER) fail("expected identifier");
        std::string result = std::move(current_.text);
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

    // EditionsStatement <- "edition" "=" StringSequence ";"
    //
    // protoc 30.1 models `edition = "2023";` as `syntax: "editions"`, which
    // this project refuses wherever linking happens. The source origin names
    // that limit here rather than leaving a generic parse error: A25's rule
    // has one classifier (`classify_file_syntax`), and a source statement the
    // parser cannot even build a descriptor from is a frontend statement, so
    // the message is written at this origin instead of routing a
    // half-built file into the linker.
    void fail_editions_statement() const
    {
        throw ParseFailure(
            current_.location,
            "editions syntax is not supported: only \"proto2\" and "
            "\"proto3\" are accepted",
            easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
    }

    // SyntaxStatement <- "syntax" "=" StringSequence ";"
    void parse_syntax()
    {
        accept_keyword("syntax");
        expect_symbol('=');
        // W-15: an explicit empty value is not the absent descriptor syntax
        // (which is proto2). Capture the value's position before the string
        // sequence consumes it, so the refusal points at the value rather
        // than at the trailing semicolon, as protoc's does.
        const SourceLocation value_location = current_.location;
        const std::string value = string_sequence();
        // W-65: the grammar comes first, exactly as protoc asks it.
        // ParseSyntaxIdentifier consumes the string, calls
        // ConsumeEndOfDeclaration(";"), and only then classifies and reports
        // the identifier (parser.cc:664-667, :684-690): a missing `;` is the
        // error even when the value would also be refused.
        expect_symbol(';');
        if (value.empty()) {
            // W-15: an explicit empty value is not the absent descriptor
            // syntax (which is proto2), and the descriptor path accepts that
            // absence, so this source-only rule has no twin to match and stays
            // DIAGNOSTIC_GENERIC (W-60's recorded bound).
            throw ParseFailure(value_location,
                               "syntax must be \"proto2\" or \"proto3\"");
        }
        // The one classifier decides whether an explicit value names a syntax
        // this project models; the literal comparison lives inside
        // classify_file_syntax (E7), not here.
        out_.file.syntax = out_.strings.save(value);
        out_.file.has_syntax = true;
        const easypb_schema::FileSyntax syntax =
            easypb_schema::classify_file_syntax(out_.file);
        if (syntax == easypb_schema::FILE_SYNTAX_UNSUPPORTED) {
            // The value token is where protoc points this refusal
            // (parser.cc:664, :686), and W-15's empty-value branch above
            // points there for the same reason (W-65).
            throw ParseFailure(value_location,
                               "syntax must be \"proto2\" or \"proto3\"",
                               easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        }
        syntax_ = syntax;
        // protoc represents proto2 by leaving FileDescriptorProto.syntax
        // absent; only proto3 is materialized in field 12.
        if (syntax == easypb_schema::FILE_SYNTAX_PROTO2) {
            out_.file.syntax = str_view();
            out_.file.has_syntax = false;
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
        out_.file.dependency.push_back(out_.strings.save(info.path));
        const int index = static_cast<int>(out_.file.dependency.size() - 1);
        if (info.modifier == ImportInfo::PUBLIC_IMPORT) {
            out_.file.public_dependency.push_back(index);
        } else if (info.modifier == ImportInfo::WEAK_IMPORT) {
            out_.file.weak_dependency.push_back(index);
        }
        out_.imports.push_back(std::move(info));
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
    // CR-28: only a field `default` may keep a leading `+` here; it is refused
    // by `apply_default` with the type-specific W-40 message (exception E1).
    // An ordinary option is parsed by protoc's `ParseOption`, which consumes
    // `-` only (`parser.cc:1508`); a leading `+` is a symbol token and reaches
    // "Expected option value." there. Parsing it here would accept the spelling
    // and then discard the value, so the refusal belongs at this shared site.
    Constant constant(bool allow_leading_plus)
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
            if (current_.symbol == '+' && !allow_leading_plus) {
                throw ParseFailure(current_.location, "Expected option value.");
            }
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
        // BoolLiteral, inf/nan, and a single Identifier all arrive as
        // identifier tokens. U-23: an option value is one identifier, never a
        // dotted path - `protoc`'s ParseOption consumes a single
        // `ConsumeIdentifier` (`parser.cc:1527-1531`) and then refuses the
        // second name part with `Expected "]"`/`Expected ";"`. Type names and
        // option names keep their own dotted rules; only the value narrows.
        if (current_.kind == TOKEN_IDENTIFIER) {
            std::string text = identifier();
            if (!sign.empty() && text != "inf" && text != "nan") {
                fail("a sign is allowed only before a numeric literal, inf, or nan");
            }
            value.text = sign + text;
            if (sign.empty() && (text == "true" || text == "false")) value.kind = Constant::BOOL_VALUE;
            else if (text == "inf" || text == "nan") value.kind = Constant::FLOAT_VALUE;
            else value.kind = Constant::IDENTIFIER_VALUE;
            return value;
        }

        // AggregateValue.  The parser only balances braces because arbitrary options
        // are not represented by the trimmed descriptor model.
        //
        // U-24: `protoc` consumes an optional leading `-` before every value and
        // then ignores it when the value is an aggregate (`parser.cc:1508`,
        // `:1593-1598`); `+` is not a value sign there and stays refused here.
        // The sign is kept for numeric literals above.
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '{' && sign != "+") {
            advance();
            value.kind = Constant::AGGREGATE_VALUE;
            value.text = "{}";
            // U-47: protoc counts with a signed `int brace_depth`
            // (`parser.cc:1430`); an unsigned 32-bit counter can wrap on a
            // crafted value. Use a checked `std::size_t` and refuse before the
            // increment would overflow.
            std::size_t depth = 1;
            while (depth != 0) {
                if (current_.kind == TOKEN_END) fail("unterminated aggregate option value");
                if (current_.kind == TOKEN_SYMBOL && current_.symbol == '{') {
                    if (depth == (std::numeric_limits<std::size_t>::max)()) {
                        fail("aggregate option value is nested too deeply");
                    }
                    ++depth;
                } else if (current_.kind == TOKEN_SYMBOL && current_.symbol == '}') {
                    --depth;
                }
                advance();
            }
            return value;
        }
        if (!sign.empty()) fail("expected value after sign");
        fail("expected option constant");
        return value;
    }

    // D2-A: the parsed Constant shape as the known-option table sees it.
    static OptionConstantKind option_constant_kind(Constant::Kind kind)
    {
        switch (kind) {
            case Constant::STRING_VALUE: return OPTION_CONSTANT_STRING;
            case Constant::INTEGER_VALUE: return OPTION_CONSTANT_INTEGER;
            case Constant::FLOAT_VALUE: return OPTION_CONSTANT_FLOAT;
            case Constant::BOOL_VALUE: return OPTION_CONSTANT_BOOL;
            case Constant::IDENTIFIER_VALUE: return OPTION_CONSTANT_IDENTIFIER;
            case Constant::AGGREGATE_VALUE: return OPTION_CONSTANT_AGGREGATE;
        }
        return OPTION_CONSTANT_IDENTIFIER;
    }

    // One door for the option table. A parenthesized (custom) name stays
    // opaque: its definition is an extension in an import this frontend does
    // not resolve, and D2-A deliberately does not implement typed custom
    // options (`OPTIONS.md`, "Schema option validation"), with one exception:
    // EasyProtoBuf's own `(easypb.cpp)` family is scoped, and `.type` is
    // meaningful only on a field (R09, U-54). Every other name must be a
    // known field of the owner's `*Options` message, must be set once unless
    // the field is repeated, and must carry the field's value kind plus the
    // value-dependent kind rules.
    void validate_option(OptionOwnerKind owner, const std::string& name,
                         const Constant& value, const SourceLocation& where,
                         const OptionFieldKind& field,
                         std::set<std::string>& seen)
    {
        if (!name.empty() && name[0] == '(') {
            const std::string scope_error = custom_option_scope_error(owner, name);
            if (!scope_error.empty()) throw ParseFailure(where, scope_error);
            return;
        }

        const KnownOption* option = find_known_option(owner, name);
        if (option == 0) {
            // protoc's sentence is `Option "foo" unknown. Ensure that your
            // proto definition file imports the proto which defines the
            // option.`; the verdict is the contract (E1/A3), so the text names
            // the option and the hint.
            throw ParseFailure(where,
                "Option \"" + name + "\" unknown; a known option may need the "
                "import that defines it");
        }
        if (!option->repeated && !seen.insert(name).second) {
            throw ParseFailure(where,
                "Option \"" + name + "\" was already set");
        }
        const std::string reason = known_option_value_error(
            *option, option_constant_kind(value.kind), value.text, field);
        if (!reason.empty()) throw ParseFailure(value.location, reason);
    }

    // OptionStatement <- "option" OptionName "=" Constant ";"
    // The trimmed descriptor model does not retain general options; the
    // statement is validated against the known-option table and discarded.
    void parse_option_statement(OptionOwnerKind owner,
                                std::set<std::string>& seen)
    {
        const SourceLocation option_location = current_.location;
        const std::string name = option_name();
        expect_symbol('=');
        const Constant value = constant(false);
        expect_symbol(';');
        validate_option(owner, name, value, option_location, OptionFieldKind(),
                        seen);
    }

    // OptionStatement in a message body. Every message option is validated like
    // any other option; the one that also changes a verdict is retained.
    //
    // R05 (U-10): `message_set_wire_format` is the message option the generator
    // cannot represent - a MessageSet is a different wire encoding - so it
    // travels in the model with its presence bit instead of being validated and
    // dropped. Both refusals protoc derives from it then answer on the source
    // door through the shared reference-free sweep: a MessageSet that declares
    // ordinary fields (U-10) and the option in a proto3 file (U-12). The option
    // statement's own position is recorded at the option record's descriptor
    // path, so a source diagnostic points at the declaration that contradicts
    // the rule.
    void parse_message_option(DescriptorProto& message,
                              const DescriptorPath& message_path,
                              std::set<std::string>& seen)
    {
        const SourceLocation option_location = current_.location;
        const std::string name = option_name();
        expect_symbol('=');
        const Constant value = constant(false);
        expect_symbol(';');
        validate_option(OPTION_OWNER_MESSAGE, name, value, option_location,
                        OptionFieldKind(), seen);
        if (name == "message_set_wire_format") {
            // validate_option has already refused anything but `true`/`false`.
            message.options.message_set_wire_format = value.text == "true";
            message.options.has_message_set_wire_format = true;
            message.has_options = true;
            out_.declaration_locations[easypb_schema::message_options_path(
                message_path)] = option_location;
        }
    }

    // RpcBody <- "{" (EmptyStatement / OptionStatement)* "}"
    // RPC metadata is intentionally not retained by the trimmed descriptor.
    void parse_rpc_body()
    {
        std::set<std::string> option_names;
        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated rpc body");
            if (accept_symbol(';')) continue;
            if (accept_keyword("option")) {
                parse_option_statement(OPTION_OWNER_METHOD, option_names);
                continue;
            }
            fail("expected option or ';' in rpc body");
        }
    }

    // Rpc <- "rpc" Identifier "(" "stream"? TypeName ")"
    //        "returns" "(" "stream"? TypeName ")" (RpcBody / ";")
    // The caller has consumed "rpc". Request/response names and method
    // options are validated syntactically; the method's name, both endpoint
    // type names and both streaming markers are retained (S5/W-16, CR-48),
    // because protoc resolves the two endpoints like field type names and
    // refuses a method whose type resolves nowhere or resolves to a
    // non-message (`CrossLinkMethod`, descriptor.cc:7714-7757), and because a
    // diagnostics mode must be able to describe the method it prints. Method
    // options are still discarded.
    MethodDescriptorProto parse_rpc(SourceLocation* name_location)
    {
        *name_location = current_.location;
        MethodDescriptorProto method;
        const std::string name = identifier();
        method.name = out_.strings.save(name);
        method.has_name = true;
        expect_symbol('(');
        // U-38: protoc writes the presence bit only for an explicitly spelled
        // `stream` (`ParseServiceMethod`, parser.cc:2281-2286, 2301-2307); a
        // unary method leaves both fields absent, so the model must too.
        method.client_streaming = accept_keyword("stream");
        method.has_client_streaming = method.client_streaming;
        const std::string input_type = full_identifier(true);
        method.input_type = out_.strings.save(input_type);
        method.has_input_type = true;
        expect_symbol(')');
        if (!accept_keyword("returns")) fail("expected 'returns' in rpc declaration");
        expect_symbol('(');
        method.server_streaming = accept_keyword("stream");
        method.has_server_streaming = method.server_streaming;
        const std::string output_type = full_identifier(true);
        method.output_type = out_.strings.save(output_type);
        method.has_output_type = true;
        expect_symbol(')');
        if (accept_symbol(';')) return method;
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '{') {
            parse_rpc_body();
            return method;
        }
        fail("expected ';' or rpc body");
        return method;
    }

    // Service <- "service" Identifier "{" ServiceElement* "}"
    // ServiceElement <- EmptyStatement / OptionStatement / Rpc
    // Services are accepted so message schemas can be consumed by Codegen.
    // The service name participates in package-scope collision validation and
    // the methods are retained (S5/W-16) so the lexical name sweep can refuse
    // a duplicate method and the linker can resolve the endpoint types; no
    // RPC API is generated.
    void parse_service()
    {
        const SourceLocation name_location = current_.location;
        const std::string name = identifier();
        ServiceDescriptorProto service;
        service.name = out_.strings.save(name);
        service.has_name = true;
        const DescriptorPath path = easypb_schema::file_service_path(
            static_cast<int>(out_.file.service.size()));
        out_.file.service.push_back(std::move(service));
        out_.declaration_locations[path] = name_location;
        expect_symbol('{');
        std::set<std::string> option_names;
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated service body");
            if (accept_symbol(';')) continue;
            if (accept_keyword("option")) {
                parse_option_statement(OPTION_OWNER_SERVICE, option_names);
                continue;
            }
            if (accept_keyword("rpc")) {
                SourceLocation method_location;
                ServiceDescriptorProto& service_slot = out_.file.service.back();
                const std::size_t method_index = service_slot.method.size();
                service_slot.method.push_back(parse_rpc(&method_location));
                out_.declaration_locations[easypb_schema::service_method_path(
                    path, static_cast<int>(method_index))] = method_location;
                continue;
            }
            fail("expected rpc, option, or ';' in service body");
        }
    }

    // FieldOptions <- "[" FieldOption ("," FieldOption)* "]"
    // The list belongs to the declaration the caller names: a field, an enum
    // value or an extension range. `default` and `json_name` are parser-level
    // pseudo-options with their own storage; `(easypb.cpp).*` is
    // EasyProtoBuf's custom option; every other name goes through the
    // known-option table (D2-A), which owns the name, the value kind and the
    // field-kind rules.
    FieldOptionState field_options(OptionOwnerKind owner,
                                   const OptionFieldKind& field_kind)
    {
        FieldOptionState state;
        if (!accept_symbol('[')) return state;
        std::set<std::string> seen;
        do {
            const SourceLocation option_location = current_.location;
            const std::string name = option_name();
            expect_symbol('=');
            // A field `default` is the one place where a leading `+` is
            // parsed and then refused by `apply_default` (W-40, exception E1).
            Constant value = constant(name == "default");
            if (name == "default" && owner == OPTION_OWNER_FIELD) {
                if (state.has_default) throw ParseFailure(option_location, "duplicate default field option");
                state.has_default = true;
                state.default_value = std::move(value);
            } else if (name == "packed" && owner == OPTION_OWNER_FIELD) {
                if (state.has_packed) throw ParseFailure(option_location, "duplicate packed field option");
                if (value.kind != Constant::BOOL_VALUE) {
                    throw ParseFailure(value.location, "packed option must be true or false");
                }
                state.has_packed = true;
                state.packed = value.text == "true";
                state.packed_location = value.location;
            } else if (name == "json_name" && owner == OPTION_OWNER_FIELD) {
                // W-43: protoc routes `json_name` to `ParseJsonName`
                // (`parser.cc:1363-1386`), which requires a string and
                // refuses a second assignment. The value is retained in the
                // model so the shared uniqueness check can compare it.
                if (state.has_json_name) {
                    throw ParseFailure(option_location,
                                       "duplicate json_name field option");
                }
                if (value.kind != Constant::STRING_VALUE) {
                    throw ParseFailure(value.location,
                                       "json_name must be a string literal");
                }
                state.has_json_name = true;
                state.json_name = std::move(value.text);
                state.json_name_location = option_location;
            } else if (name.compare(0, 12, "(easypb.cpp)") == 0 &&
                       owner != OPTION_OWNER_FIELD) {
                // R09 (U-54): EasyProtoBuf's C++ type option is a field
                // option. The branch below would otherwise retain it in a
                // state the enum-value and extension-range callers discard, so
                // a non-field scope must refuse it and name both the option
                // and the scope.
                throw ParseFailure(option_location,
                    custom_option_scope_error(owner, name));
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
                state.cpp_type = std::move(value.text);
            } else if (name == "(easypb.cpp)") {
                throw ParseFailure(option_location,
                    "use (easypb.cpp).type = \"...\" instead of assigning "
                    "(easypb.cpp) directly");
            } else if (name.compare(0, 13, "(easypb.cpp).") == 0) {
                throw ParseFailure(option_location,
                    "unknown EasyProtoBuf C++ field option " + name);
            } else {
                validate_option(owner, name, value, option_location,
                                field_kind, seen);
                // U-08: the value decides, not the presence. `lazy = false` and
                // `unverified_lazy = false` are the option defaults and are
                // legal on every field kind; only `true` reaches the deferred
                // message-only rule in `validate_field_encoding`.
                if ((name == "lazy" || name == "unverified_lazy") &&
                    value.text == "true") {
                    state.has_lazy = true;
                    state.lazy_location = option_location;
                }
                if (owner == OPTION_OWNER_EXTENSION_RANGE) {
                    if (name == "declaration") {
                        state.has_declaration = true;
                    } else if (name == "verification") {
                        state.has_verification = true;
                        state.verification = value.text;
                        state.verification_location = option_location;
                    }
                }
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

    // FieldRangeNumber <- IntegerToken.  Unlike an actual field number,
    // a range endpoint may include the protoc-reserved 19000..19999, and an
    // extension range of a MessageSet may reach INT32_MAX - 1 (V-05). The
    // ordinary-message cap is applied after the message body, where
    // `message_set_wire_format` is known.
    std::int32_t field_range_number()
    {
        if (current_.kind != TOKEN_INTEGER) fail("expected positive field number");
        const SourceLocation where = current_.location;
        std::uint64_t number = 0;
        if (!easypb_schema::parse_unsigned_integer(current_.text, number) || number == 0 ||
            number > static_cast<std::uint64_t>(kMaxMessageSetNumber)) {
            throw ParseFailure(where,
                "extension range endpoint must be in range 1..2147483646");
        }
        advance();
        return static_cast<std::int32_t>(number);
    }

    // V-05: an extension field's tag is not capped at
    // FieldDescriptor::kMaxNumber. protoc's descriptor builder skips that
    // bound for extension fields (`descriptor.cc:6807-6822`) and validates the
    // number against the extendee's own extension ranges instead (`:7451-7463`),
    // and a MessageSet declares ranges up to INT32_MAX - 1. The trimmed model
    // discards the member (E6), so the parser keeps the widest int32 bound a
    // tag can carry.
    std::int32_t extension_field_number()
    {
        if (current_.kind != TOKEN_INTEGER) fail("expected positive field number");
        const SourceLocation where = current_.location;
        std::uint64_t number = 0;
        if (!easypb_schema::parse_unsigned_integer(current_.text, number) || number == 0 ||
            number > static_cast<std::uint64_t>(kMaxMessageSetNumber)) {
            throw ParseFailure(where,
                "extension number must be in range 1..2147483646");
        }
        if (number >= 19000ull && number <= 19999ull) {
            throw ParseFailure(where, "field numbers 19000..19999 are reserved by Protocol Buffers");
        }
        advance();
        return static_cast<std::int32_t>(number);
    }

    // U-56 / V-03: a reserved declaration's inclusive endpoints may reach
    // INT32_MAX - 1 (2147483646). protoc stores the range as [start, end) and
    // its builder asks only that start is positive and smaller than end
    // (descriptor.cc:6906-6922), so 2147483646 is the largest spellable
    // endpoint - "2147483647" would overflow the exclusive end. The descriptor
    // door already accepts this bound. A *field* number keeps the 536870911
    // cap that positive_field_number enforces.
    std::int32_t reserved_range_number()
    {
        if (current_.kind != TOKEN_INTEGER) fail("expected positive field number");
        const SourceLocation where = current_.location;
        std::uint64_t number = 0;
        if (!easypb_schema::parse_unsigned_integer(current_.text, number) || number == 0 ||
            number > static_cast<std::uint64_t>(kMaxMessageSetNumber)) {
            throw ParseFailure(where,
                "reserved range endpoint must be in range 1..2147483646");
        }
        advance();
        return static_cast<std::int32_t>(number);
    }

    // SignedInteger <- Sign? IntegerToken, constrained to int32.
    std::int32_t signed_enum_number()
    {
        std::string sign;
        const SourceLocation where = current_.location;
        // W-40: protoc's `ConsumeSignedInteger` consumes `-` only
        // (`parser.cc:191-203`), so a leading `+` is not a sign here. Keep
        // `-` accepted and refuse `+` at the value, before consuming it.
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '+') {
            throw ParseFailure(where, "Expected integer.");
        }
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '-') {
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
        if (syntax_ == easypb_schema::FILE_SYNTAX_PROTO3) {
            throw ParseFailure(value.location,
                               "explicit default values are not allowed in proto3",
                               easypb_schema::DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT);
        }
        // CR-11: protoc accepts an explicit default on a oneof member.
        // `ParseDefaultAssignment` (`parser.cc:1225-1361`) has no oneof
        // clause, and the descriptor it writes is an optional field with
        // `oneof_index`; the descriptor path already accepts that shape. Only
        // the repeated-field refusal stays here.
        if (field.label == FieldDescriptorProto::LABEL_REPEATED) {
            throw ParseFailure(value.location, "default value is not allowed on repeated fields");
        }

        // W-40 (option B): refuse every leading `+` on a numeric constant
        // before the type switch. `constant(true)` parses the sign only for
        // this caller, so the error points at the value with the type-specific
        // message; every ordinary option passes `false` and refuses the sign
        // as a grammar error (CR-28). `protoc` calls `ConsumeInteger64` /
        // `ConsumeNumber` without a unary-plus branch (`parser.cc:1272`,
        // `:1312`), so these spellings never reach its value parsers either.
        if ((value.kind == Constant::INTEGER_VALUE ||
             value.kind == Constant::FLOAT_VALUE) &&
            !value.text.empty() && value.text[0] == '+') {
            if (field.type == FieldDescriptorProto::TYPE_FLOAT ||
                field.type == FieldDescriptorProto::TYPE_DOUBLE) {
                throw ParseFailure(value.location, "Expected number.");
            }
            if (!field.has_type && field.has_type_name) {
                throw ParseFailure(value.location, "Expected integer.");
            }
            throw ParseFailure(value.location,
                               "Expected integer for field default value.");
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
            // W-04: the shared canonicalizer owns the ERANGE family now
            // (`1e400` -> inf, `1e-400` -> 0, `-1e-400` -> -0 and finite
            // subnormals -> their shortest decimal), so the parser and the
            // descriptor adapter store the same text.
            //
            // CR-01: an INTEGER token is not decimal text. protoc reads it
            // with the octal/hex-aware integer parser and converts the value
            // to double (`parser.cc:231-255`, `:1265-1307`), which is why
            // `[default = 010]` means 8, not 10. The FLOAT token keeps the
            // decimal path.
            const bool single_precision =
                field.type == FieldDescriptorProto::TYPE_FLOAT;
            if (value.kind == Constant::INTEGER_VALUE) {
                double numeric = 0;
                if (!easypb_schema::integer_token_as_double(value.text,
                                                            numeric)) {
                    throw ParseFailure(value.location, "Integer out of range.");
                }
                if (!easypb_schema::canonical_float_default(
                        numeric, single_precision, stored)) {
                    throw ParseFailure(value.location,
                                       "invalid floating-point default");
                }
            } else if (!easypb_schema::canonical_float_default(
                           value.text, single_precision, stored)) {
                throw ParseFailure(value.location,
                                   "invalid floating-point default");
            }
        } else if (field.has_type_name) {
            // V-04: protoc's enum-default branch accepts any identifier token
            // (`ParseDefaultAssignment`, parser.cc:1330-1340); its tokenizer
            // has no separate `true`/`false`/`inf`/`nan` token kind, so those
            // spellings are legal enum value names and legal defaults for
            // them. EasyPB's Constant classifies the four literals as
            // BOOL_VALUE/FLOAT_VALUE, so the enum branch admits those two
            // shapes and keeps the identifier rule for every other kind
            // (a numeric float stays refused, and so does a signed literal).
            const bool keyword_constant =
                value.kind == Constant::BOOL_VALUE ||
                (value.kind == Constant::FLOAT_VALUE &&
                 (value.text == "inf" || value.text == "nan"));
            if (value.kind != Constant::IDENTIFIER_VALUE && !keyword_constant) {
                throw ParseFailure(value.location, "enum default must be an identifier");
            }
            stored = value.text;
        } else {
            if (value.kind != Constant::INTEGER_VALUE) {
                throw ParseFailure(value.location, "integral default must be an integer literal");
            }
            // CR-17: both frontends store the decimal value, exactly as
            // protoc's descriptor builder re-renders it, so `010` and `0x10`
            // cannot reach the header as their source spelling. A text the
            // shared parser rejects (`08`, an out-of-range magnitude) keeps
            // its spelling and is refused by validate_default, which owns the
            // range diagnostics.
            if (!easypb_schema::canonical_integral_default(value.text, stored)) {
                stored = value.text;
            }
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
        if (provenance.has_lazy) {
            source.has_lazy = true;
            source.lazy_location = provenance.lazy_location;
        } else {
            source.lazy_location = easypb_schema::unknown_location();
        }
        out_.declaration_locations[source.path] = provenance.name_location;
        out_.field_sources.push_back(std::move(source));
    }

    // Parses one field production, appends it to message.field, and records
    // its source provenance.
    void push_field(DescriptorProto& message, const DescriptorPath& message_path,
                    bool oneof_field, std::int32_t oneof_index)
    {
        FieldProvenance provenance;
        message.field.push_back(parse_field(oneof_field, oneof_index, &provenance, false));
        record_field_source(message_path, message.field.size() - 1, provenance);
    }

    // Field      <- Label? TypeName Identifier "=" FieldNumber
    //               FieldOptions? ";"
    // OneofField <- TypeName Identifier "=" FieldNumber FieldOptions? ";"
    // oneof_field selects the second production and records oneof_index.
    // extension_field marks a field inside an `extend` body: its json_name is
    // restricted to the derived spelling (U-25), because protoc refuses a
    // custom name on an extension field.
    // Provenance (raw type spelling and option locations) is reported through
    // the out-parameter; extend declarations pass null because their fields
    // are validated but not retained.
    FieldDescriptorProto parse_field(bool oneof_field, std::int32_t oneof_index,
                                     FieldProvenance* provenance,
                                     bool extension_field)
    {
        FieldDescriptorProto field;

        // Label? (absent for OneofField and permitted to be absent in proto3).
        bool label_seen = false;
        bool explicit_optional = false;
        if (is_keyword("optional") || is_keyword("required") || is_keyword("repeated")) {
            if (oneof_field) fail("oneof fields must not have optional, required, or repeated labels");
            label_seen = true;
            if (accept_keyword("optional")) {
                field.label = FieldDescriptorProto::LABEL_OPTIONAL;
                explicit_optional = true;
            }
            else if (accept_keyword("required")) field.label = FieldDescriptorProto::LABEL_REQUIRED;
            else {
                accept_keyword("repeated");
                field.label = FieldDescriptorProto::LABEL_REPEATED;
            }
        } else {
            field.label = FieldDescriptorProto::LABEL_OPTIONAL;
        }
        field.has_label = true;

        if (syntax_ == easypb_schema::FILE_SYNTAX_PROTO2 &&
            !oneof_field && !label_seen) {
            fail("proto2 fields require optional, required, or repeated label");
        }
        if (syntax_ == easypb_schema::FILE_SYNTAX_PROTO3 &&
            field.label == FieldDescriptorProto::LABEL_REQUIRED) {
            fail("required fields are not allowed in proto3",
                 easypb_schema::DIAGNOSTIC_PROTO3_REQUIRED_FIELD);
        }
        // D1-A: in proto3 only an explicit `optional` label carries presence.
        // An absent label is an ordinary implicit field and must stay without
        // the flag; proto2 never sets it (`ParseMessageField`,
        // parser.cc:944-955).
        if (syntax_ == easypb_schema::FILE_SYNTAX_PROTO3 && explicit_optional) {
            field.proto3_optional = true;
            field.has_proto3_optional = true;
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
        field.number = extension_field ? extension_field_number()
                                       : positive_field_number();
        field.has_number = true;

        // D2-A: the field options are validated against the table once the
        // field's declared kind is known, so `jstype` and `lazy` can be
        // decided without a second parse.
        OptionFieldKind field_kind;
        field_kind.has_type = field.has_type;
        field_kind.type = field.type;
        const FieldOptionState options = field_options(OPTION_OWNER_FIELD,
                                                       field_kind);
        // U-25: protoc's `ValidateFieldOptions` refuses a json_name on an
        // extension field when it differs from `ToJsonName(field->name())`
        // (`descriptor.cc:8023-8037`); the equal spelling is the derived name
        // and stays accepted.
        if (extension_field && options.has_json_name &&
            options.json_name != easypb_schema::to_json_name(name)) {
            throw ParseFailure(options.json_name_location,
                "option json_name is not allowed on extension fields.");
        }
        expect_symbol(';');

        if (oneof_field) {
            field.oneof_index = oneof_index;
            field.has_oneof_index = true;
        }
        if (options.has_default) apply_default(field, options.default_value);
        if (options.has_json_name) {
            field.json_name = out_.strings.save(options.json_name);
            field.has_json_name = true;
        }
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
            provenance->has_lazy = options.has_lazy;
            if (options.has_lazy) {
                provenance->lazy_location = options.lazy_location;
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
        // A map field is a repeated synthetic-entry message: a message kind,
        // so `jstype` is refused while `lazy = true` stays legal for the
        // shared field pass.
        OptionFieldKind field_kind;
        field_kind.has_type = true;
        field_kind.type = FieldDescriptorProto::TYPE_MESSAGE;
        const FieldOptionState options = field_options(OPTION_OWNER_FIELD,
                                                       field_kind);
        expect_symbol(';');
        if (options.has_default || (options.has_packed && options.packed)) {
            // U-08: `protoc` asks `options().packed()` (value, not presence),
            // so an explicit `packed = false` on a map is the default and is
            // accepted; `packed = true` stays refused because a map field is a
            // repeated message, which is not packable.
            // Point at the offending option value, not at the key type.
            const SourceLocation option_location = options.has_default
                ? options.default_value.location
                : options.packed_location;
            throw ParseFailure(option_location, "map fields cannot have default or packed options");
        }

        // Synthesize the nested map-entry descriptor expected by codegen. The
        // entry's name follows protoc's rule, which is one shared function
        // because the linker has to derive the same name without a parser
        // (amendment A24).
        const std::string entry_name = easypb_schema::map_entry_name(field_name);
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
        entry.field.push_back(std::move(key));

        // Entry field 2: optional TypeName value.
        FieldDescriptorProto value;
        value.name = out_.strings.save("value");
        value.has_name = true;
        value.number = 2;
        value.has_number = true;
        value.label = FieldDescriptorProto::LABEL_OPTIONAL;
        value.has_label = true;
        set_type(value, value_type_name);
        entry.field.push_back(std::move(value));

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
        // W-43: `json_name` is a property of the source map field too, even
        // though the field's type is the synthetic entry. Keep it beside the
        // repeated descriptor field so the shared JSON-name sweep sees the
        // same custom name protoc sees.
        if (options.has_json_name) {
            field.json_name = out_.strings.save(options.json_name);
            field.has_json_name = true;
        }

        message.nested_type.push_back(std::move(entry));
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
            out_.field_sources.push_back(std::move(key_source));
        }
        {
            FieldSource value_source;
            value_source.path = value_path;
            value_source.raw_type_name = value_type_name;
            value_source.type_location = value_location;
            value_source.default_location = easypb_schema::unknown_location();
            value_source.packed_location = easypb_schema::unknown_location();
            out_.field_sources.push_back(std::move(value_source));
        }
        message.field.push_back(std::move(field));
        {
            FieldSource field_source;
            field_source.path = easypb_schema::message_field_path(
                message_path, static_cast<int>(message.field.size() - 1));
            field_source.raw_type_name = entry_name;
            field_source.type_location = map_location;
            field_source.default_location = easypb_schema::unknown_location();
            field_source.packed_location = easypb_schema::unknown_location();
            out_.declaration_locations[field_source.path] = field_name_location;
            out_.field_sources.push_back(std::move(field_source));
        }
    }

    // D1-A: one synthetic one-field oneof per proto3 `optional` field, named
    // and ordered exactly as `protoc`'s `GenerateSyntheticOneofs`
    // (`parser.cc:745-780`) names and orders them:
    //   1. the collision set is every field name and every oneof name;
    //   2. the candidate starts as the field name and gains one leading `_`
    //      unless it already starts with one; a field whose own name is the
    //      candidate collides and is resolved by step 3;
    //   3. while the candidate collides, prepend one `X` (`X_a`, `XX_a`, ...);
    //   4. the field claims the declaration, which is appended after every
    //      real oneof because the whole message body has been parsed.
    // The declaration location is the field's name: that is where a reader
    // looks for the presence declaration, and it keeps diagnostics inside the
    // message rather than at the closing brace.
    void synthesize_proto3_oneofs(DescriptorProto& message,
                                  const DescriptorPath& message_path)
    {
        std::set<std::string> names;
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            names.insert(view_text(message.field[i].name));
        }
        for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
            names.insert(view_text(message.oneof_decl[i].name));
        }

        for (std::size_t i = 0; i < message.field.size(); ++i) {
            FieldDescriptorProto& field = message.field[i];
            if (!field.has_proto3_optional || !field.proto3_optional) continue;
            // A field inside a real oneof cannot carry a label, so it can
            // never reach this point with the flag; the guard keeps the
            // invariant local instead of relying on that refusal.
            if (field.has_oneof_index) continue;

            std::string oneof_name = view_text(field.name);
            if (oneof_name.empty() || oneof_name[0] != '_') {
                oneof_name.insert(0, 1, '_');
            }
            while (names.count(oneof_name) != 0) {
                oneof_name.insert(0, 1, 'X');
            }
            names.insert(oneof_name);

            field.oneof_index =
                static_cast<std::int32_t>(message.oneof_decl.size());
            field.has_oneof_index = true;

            OneofDescriptorProto declaration;
            declaration.name = out_.strings.save(oneof_name);
            declaration.has_name = true;
            const std::int32_t index =
                static_cast<std::int32_t>(message.oneof_decl.size());
            message.oneof_decl.push_back(std::move(declaration));

            const DescriptorPath field_path =
                easypb_schema::message_field_path(message_path,
                                                  static_cast<int>(i));
            const std::map<DescriptorPath, SourceLocation>::const_iterator
                located = out_.declaration_locations.find(field_path);
            if (located != out_.declaration_locations.end()) {
                out_.declaration_locations[easypb_schema::message_oneof_path(
                    message_path, index)] = located->second;
            }
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
        // The nesting bound, before the function descends into the body. The
        // reference compiler refuses the 32nd level of nested messages with
        // "Reached maximum recursion limit for nested messages."
        // (compiler/parser.cc:785-789; its limit is
        // MaxMessageDeclarationNestingDepth() == 32, descriptor.h:3045), and
        // the depth of a message is the number of levels of its descriptor
        // path: the path holds a field number and an index per level
        // (easypb_schema::file_message_path, message_nested_path), so a
        // file-level message has two components and a message nested in it
        // has four.
        //
        // The check stands here so that the parser never descends deeper than
        // the limit it enforces - the recursion below is what overflowed the
        // stack on a deep schema.
        if (message_path.size() / 2 >=
                static_cast<std::size_t>(
                    easypb_schema::max_message_nesting_depth())) {
            fail("Reached maximum recursion limit for nested messages.");
        }

        const SourceLocation name_location = current_.location;
        const std::string name = identifier();
        DescriptorProto message;
        message.name = out_.strings.save(name);
        message.has_name = true;

        std::vector<NumberRange> reserved_ranges;
        std::set<std::string> reserved_names;
        std::vector<NumberRange> extension_ranges;
        std::set<std::string> option_names;
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
            else if (accept_keyword("option"))
                parse_message_option(message, message_path, option_names);                  // OptionStatement
            else if (accept_keyword("reserved")) parse_reserved(false, reserved_ranges, reserved_names); // ReservedMessage
            else if (accept_keyword("extensions")) parse_extensions(extension_ranges);            // Extensions
            else if (accept_keyword("extend")) parse_extend();                               // Extend
            else if (is_keyword("service")) fail("service declarations are not allowed inside messages");
            else push_field(message, message_path, false, 0);                                    // Field
        }
        synthesize_proto3_oneofs(message, message_path);
        validate_message_constraints(message, message_path, name_location,
                                    reserved_ranges, reserved_names,
                                    extension_ranges);
        return message;
    }

    // Oneof <- "oneof" Identifier "{"
    //          (OptionStatement / OneofField)* "}"
    // The caller has consumed "oneof".  Fields are appended to message.field
    // and linked to the newly appended declaration through oneof_index.
    // message_path is the owning message's descriptor path.
    //
    // V-08: the body has no EmptyStatement production. protoc's `ParseOneof`
    // (parser.cc:2022-2085) parses every non-option statement as a field, so a
    // bare `;` fails as `Expected type name.`; EasyPB used to skip it.
    void parse_oneof(DescriptorProto& message,
                     const DescriptorPath& message_path)
    {
        const SourceLocation name_location = current_.location;
        const std::string oneof_name = identifier();
        for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
            if (message.oneof_decl[i].name == oneof_name) {
                throw ParseFailure(name_location, "duplicate oneof name " + oneof_name);
            }
        }
        OneofDescriptorProto declaration;
        declaration.name = out_.strings.save(oneof_name);
        declaration.has_name = true;
        const std::int32_t index = static_cast<std::int32_t>(message.oneof_decl.size());
        message.oneof_decl.push_back(std::move(declaration));
        out_.declaration_locations[easypb_schema::message_oneof_path(
            message_path, index)] = name_location;
        expect_symbol('{');
        std::set<std::string> option_names;
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated oneof body");

            if (current_.kind == TOKEN_SYMBOL && current_.symbol == ';') {
                fail("Expected type name.");
            }
            // Oneof body alternatives: OptionStatement or OneofField.
            // MapField is recognized explicitly to produce a more useful
            // diagnostic than the generic field parser would.
            if (accept_keyword("option"))
                parse_option_statement(OPTION_OWNER_ONEOF, option_names);
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
        // CR-42: the post-body rules below used `current_`, which is the token
        // after the closing brace by the time the body has been consumed. The
        // enum's own name and every value's number are captured while they are
        // in front of the cursor, so each refusal points inside the
        // declaration - at the name, or at the value that breaks the rule.
        const SourceLocation enum_name_location = current_.location;
        const std::string name = identifier();
        EnumDescriptorProto result;
        result.name = out_.strings.save(name);
        result.has_name = true;
        bool allow_alias = false;
        bool has_allow_alias = false;
        SourceLocation allow_alias_location;
        std::set<std::string> value_names;
        std::vector<std::int32_t> numbers;
        std::vector<SourceLocation> number_locations;
        std::vector<SourceLocation> value_locations;
        std::vector<NumberRange> reserved_ranges;
        std::set<std::string> reserved_names;
        std::set<std::string> option_names;

        expect_symbol('{');
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated enum body");

            // EnumElement <- EmptyStatement / EnumOption / ReservedEnum /
            //                EnumValue
            if (accept_symbol(';')) continue;                         // EmptyStatement
            if (accept_keyword("option")) {                           // EnumOption
                const SourceLocation option_location = current_.location;
                const std::string option = option_name();
                expect_symbol('=');
                const Constant value = constant(false);
                expect_symbol(';');
                validate_option(OPTION_OWNER_ENUM, option, value,
                                option_location, OptionFieldKind(),
                                option_names);
                if (option == "allow_alias") {
                    if (value.kind != Constant::BOOL_VALUE) {
                        throw ParseFailure(value.location, "allow_alias must be true or false");
                    }
                    allow_alias = value.text == "true";
                    has_allow_alias = true;
                    allow_alias_location = option_location;
                    // D-28 (fix round r13): the option travels in the model,
                    // because the linker validates descriptor-origin enums for
                    // repeated numbers and cannot tell an intentional alias
                    // from that defect without it. Source text is refused below
                    // when the option is absent, so recording it here is what
                    // keeps an aliased source file (`enum_codegen.proto`'s
                    // `STARTED = 1; ACTIVE = 1;`) linking.
                    result.options.allow_alias = allow_alias;
                    result.options.has_allow_alias = true;
                    result.has_options = true;
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
            value_locations.push_back(value_location);
            expect_symbol('=');
            EnumValueDescriptorProto value;
            value.name = out_.strings.save(value_name);
            value.has_name = true;
            const SourceLocation number_location = current_.location;
            value.number = signed_enum_number();
            value.has_number = true;
            numbers.push_back(value.number);
            number_locations.push_back(number_location);
            if (reserved_names.find(value_name) != reserved_names.end()) {
                throw ParseFailure(value_location, "enum value name is reserved: " + value_name);
            }
            for (std::size_t r = 0; r < reserved_ranges.size(); ++r) {
                if (range_contains(reserved_ranges[r], value.number)) {
                    throw ParseFailure(value_location, "enum value number is reserved");
                }
            }
            if (current_.kind == TOKEN_SYMBOL && current_.symbol == '[') {
                // CR-27/R14 (U-49): the call is the whole check. `default`,
                // `packed` and `json_name` are not fields of
                // `EnumValueOptions`, so the known-option table refuses each
                // one as unknown (protoc's `ParseOption` does the same), and
                // `(easypb.cpp).type` is refused by the field-scope rule
                // (U-54). The former post-check on the state could never fire.
                (void)field_options(OPTION_OWNER_ENUM_VALUE, OptionFieldKind());
            }
            expect_symbol(';');
            out_.declaration_locations[easypb_schema::enum_value_path(
                enum_path, static_cast<int>(result.value.size()))] = value_location;
            result.value.push_back(std::move(value));
        }

        // CR-26: protoc's parser validates `allow_alias` before the descriptor
        // builder sees the enum (`ValidateEnum`, parser.cc:520-575). An
        // explicit `false` has no effect, and `true` without a shared number is
        // unnecessary; both are refusals. The rules are source-text rules:
        // protoc's descriptor path accepts the same options, so the descriptor
        // path here stays at parity and does not grow a check of its own.
        if (has_allow_alias && !allow_alias) {
            throw ParseFailure(allow_alias_location,
                "\"" + name + "\" declares 'option allow_alias = false;' which "
                "has no effect. Please remove the declaration.");
        }
        bool has_duplicate_number = false;
        std::size_t duplicate_number_index = numbers.size();
        {
            std::map<std::int32_t, std::size_t> first_by_number;
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                const std::pair<std::map<std::int32_t, std::size_t>::iterator,
                                bool> inserted = first_by_number.insert(
                    std::map<std::int32_t, std::size_t>::value_type(numbers[i], i));
                if (! inserted.second) {
                    has_duplicate_number = true;
                    // protoc reports the first value that repeats a number, so
                    // the diagnostic names the later declaration.
                    if (duplicate_number_index == numbers.size()) {
                        duplicate_number_index = i;
                    }
                }
            }
        }
        if (allow_alias && !has_duplicate_number) {
            throw ParseFailure(allow_alias_location,
                "\"" + name + "\" declares support for enum aliases but no enum "
                "values share field numbers. Please remove the unnecessary "
                "'option allow_alias = true;' declaration.");
        }

        if (result.value.empty()) {
            throw ParseFailure(enum_name_location,
                               "enum must contain at least one value",
                               easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
        }

        if (!reserved_names.empty() || !reserved_ranges.empty()) {
            // U-32: this re-check runs after the closing brace, so `fail()`
            // pointed at the token following the enum. The declaration
            // positions were captured while each value was in front of the
            // cursor, so the refusal names the value that reuses a reserved
            // name or number instead.
            for (std::size_t i = 0; i < result.value.size(); ++i) {
                if (!reserved_names.empty()) {
                    const std::string value_name = view_text(result.value[i].name);
                    if (reserved_names.find(value_name) != reserved_names.end()) {
                        throw ParseFailure(value_locations[i],
                            "enum value name is reserved: " + value_name);
                    }
                }
                for (std::size_t r = 0; r < reserved_ranges.size(); ++r) {
                    if (range_contains(reserved_ranges[r], result.value[i].number)) {
                        throw ParseFailure(value_locations[i],
                            "enum value number is reserved");
                    }
                }
            }
        }

        if (syntax_ == easypb_schema::FILE_SYNTAX_PROTO3 &&
            !result.value.empty() && result.value[0].number != 0) {
            throw ParseFailure(number_locations[0],
                               "the first proto3 enum value must be zero");
        }
        if (!allow_alias && has_duplicate_number) {
            throw ParseFailure(
                number_locations[duplicate_number_index],
                "duplicate enum number requires option allow_alias = true",
                easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);
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
                // U-50: protoc 30.1 accepts any string literal here but warns
                // when it is not an identifier ("Reserved name \"%s\" is not a
                // valid identifier.", parser.cc:1767-1777). The verdict stays
                // accept; the warning is the whole rule.
                if (!easypb_schema::is_proto_identifier(name)) {
                    Diagnostic warning;
                    warning.file = file_name_;
                    warning.location = where;
                    warning.warning = true;
                    warning.message =
                        "Reserved name \"" + name +
                        "\" is not a valid identifier.";
                    out_.warnings.push_back(std::move(warning));
                }
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
            range.first = enum_context ? signed_enum_number() : reserved_range_number();
            range.last = range.first;
            if (accept_keyword("to")) {
                if (accept_keyword("max")) {
                    range.last = enum_context ? static_cast<std::int64_t>(INT32_MAX)
                                              : static_cast<std::int64_t>(536870911);
                } else {
                    range.last = enum_context ? signed_enum_number() : reserved_range_number();
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
    // V-05: `N to max` keeps the widest inclusive endpoint until the message
    // body ends, because a MessageSet's `to max` is 2147483646 while an
    // ordinary message's is 536870911 (protoc's
    // `AdjustExtensionRangesWithMaxEndNumber`, parser.cc:827-837).
    void parse_extensions(std::vector<NumberRange>& ranges)
    {
        if (syntax_ != easypb_schema::FILE_SYNTAX_PROTO2) fail("extensions ranges are allowed only in proto2");
        for (;;) {
            NumberRange range;
            range.location = current_.location;
            range.first = field_range_number();
            range.last = range.first;
            if (accept_keyword("to")) {
                if (accept_keyword("max")) {
                    range.last = static_cast<std::int64_t>(kMaxMessageSetNumber);
                    range.max_end = true;
                } else {
                    range.last = field_range_number();
                }
            }
            add_range(ranges, range, "extension");
            if (!accept_symbol(',')) break;
        }
        if (current_.kind == TOKEN_SYMBOL && current_.symbol == '[') {
            const FieldOptionState options =
                field_options(OPTION_OWNER_EXTENSION_RANGE, OptionFieldKind());
            if (options.has_declaration && options.has_verification &&
                options.verification == "UNVERIFIED") {
                throw ParseFailure(options.verification_location,
                    "cannot mark the extension range as UNVERIFIED when it has "
                    "extensions declared");
            }
        }
        expect_symbol(';');
    }

    // Post-action for Message: checks relationships among parsed fields,
    // ReservedMessage declarations, and Extensions declarations.
    //
    // CR-42: this runs after the closing brace, so `current_` is the token
    // after the declaration. The field rules below point at the field they are
    // about: every field records its name position in the shared declaration
    // table (the same entry the descriptor path uses), and that position is
    // inside the message. `message_location` is the message name, the fallback
    // for a field whose entry is somehow missing - still inside the
    // declaration, never the token after the brace.
    void validate_message_constraints(const DescriptorProto& message,
                                      const DescriptorPath& message_path,
                                      const SourceLocation& message_location,
                                      const std::vector<NumberRange>& reserved_ranges,
                                      const std::set<std::string>& reserved_names,
                                      const std::vector<NumberRange>& extension_ranges)
    {
        if (reserved_names.empty() && reserved_ranges.empty() &&
            extension_ranges.empty()) {
            return;
        }

        // V-05: resolve `to max` now that the whole message body has been
        // parsed, and apply the ordinary-message cap. A MessageSet's extension
        // numbers are plain int32s (`max_extension_range`,
        // descriptor.cc:8385-8389), so its ranges may reach INT32_MAX - 1;
        // every other message keeps FieldDescriptor::kMaxNumber. protoc's
        // parser accepts an explicit INT32_MAX - 1 endpoint for any message and
        // refuses only the expanded range at build time, which is why the check
        // lives here rather than beside `field_range_number`.
        std::vector<NumberRange> resolved_ranges = extension_ranges;
        const bool message_set = message.options.message_set_wire_format;
        for (std::size_t i = 0; i < resolved_ranges.size(); ++i) {
            NumberRange& range = resolved_ranges[i];
            if (range.max_end) {
                range.last = message_set ? static_cast<std::int64_t>(kMaxMessageSetNumber)
                                         : static_cast<std::int64_t>(536870911);
            }
            if (!message_set && range.last > 536870911) {
                // The sentence CR-07's parity pair already pins: the ordinary
                // message cap is unchanged, only the verdict's position moved
                // from the token to the end of the body.
                throw ParseFailure(range.location,
                    "field number must be in range 1..536870911");
            }
        }

        for (std::size_t i = 0; i < reserved_ranges.size(); ++i) {
            for (std::size_t j = 0; j < resolved_ranges.size(); ++j) {
                if (ranges_overlap(reserved_ranges[i], resolved_ranges[j])) {
                    throw ParseFailure(resolved_ranges[j].location,
                        "reserved and extension ranges overlap");
                }
            }
        }

        for (std::size_t i = 0; i < message.field.size(); ++i) {
            const FieldDescriptorProto& field = message.field[i];
            const std::map<DescriptorPath, SourceLocation>::const_iterator
                located = out_.declaration_locations.find(
                    easypb_schema::message_field_path(
                        message_path, static_cast<int>(i)));
            // Every field path is recorded while the field is parsed; the
            // fallback keeps an incomplete table from producing an unlocated
            // diagnostic and still names a position inside the message.
            const SourceLocation field_location =
                located == out_.declaration_locations.end()
                    ? message_location : located->second;
            if (!reserved_names.empty()) {
                const std::string name = view_text(field.name);
                if (reserved_names.find(name) != reserved_names.end()) {
                    throw ParseFailure(field_location,
                        "field name is reserved: " + name);
                }
            }
            for (std::size_t j = 0; j < reserved_ranges.size(); ++j) {
                if (range_contains(reserved_ranges[j], field.number)) {
                    throw ParseFailure(field_location,
                        "field number is reserved");
                }
            }
            for (std::size_t j = 0; j < resolved_ranges.size(); ++j) {
                if (range_contains(resolved_ranges[j], field.number)) {
                    throw ParseFailure(field_location,
                        "field number lies inside an extension range");
                }
            }
        }
    }

    // D-01: an `extend` body must contain at least one field. protoc's
    // `ParseExtend` is a `do { ... } while (!TryConsumeEndOfDeclaration("}"))`
    // loop (parser.cc:1979-2010), so an immediate `}` or `;` is parsed as a
    // field and fails with `Expected "required", "optional", or "repeated".`
    // (parser.cc:996-997). The E6 warning below therefore describes only a
    // non-empty, valid block.
    //
    // Extend <- "extend" TypeName "{" Field+ "}"
    // The syntax is validated, but fields are not retained because the trimmed
    // FileDescriptorProto has no extension collection. U-16/V-12: proto3
    // accepts this declaration only for the nine descriptor-option extendees
    // protoc's `AllowedExtendeeInProto3` names after resolving the spelling;
    // every other extendee keeps the proto2 refusal. The verdict is recorded
    // for `validate_proto3_extends`, because the package that qualifies the
    // name may be declared later in the file. The accepted body is discarded
    // with the E6 warning, exactly as the proto2 path discards it.
    void parse_extend()
    {
        const SourceLocation where = current_.location;
        const std::string extendee = full_identifier(true);
        if (syntax_ != easypb_schema::FILE_SYNTAX_PROTO2) {
            Proto3ExtendCheck check;
            check.spelling = extendee;
            check.location = where;
            proto3_extends_.push_back(std::move(check));
        }
        expect_symbol('{');
        if (current_.kind == TOKEN_SYMBOL &&
            (current_.symbol == '}' || current_.symbol == ';')) {
            fail("Expected \"required\", \"optional\", or \"repeated\".");
        }
        while (!accept_symbol('}')) {
            if (current_.kind == TOKEN_END) fail("unterminated extend body");
            (void)parse_field(false, 0, 0, true);
        }
        Diagnostic warning;
        warning.file = file_name_;
        warning.location = where;
        warning.warning = true;
        warning.message = "extend declaration parsed but not stored by the trimmed FileDescriptorProto model";
        out_.warnings.push_back(std::move(warning));
    }

    // V-12: runs once the file body is complete, because `package` may follow
    // the `extend` statement it qualifies.
    void validate_proto3_extends()
    {
        if (proto3_extends_.empty()) return;
        const std::string package =
            out_.file.has_package ? view_text(out_.file.package) : std::string();
        for (std::size_t i = 0; i < proto3_extends_.size(); ++i) {
            if (!allowed_proto3_extendee(proto3_extends_[i].spelling, package)) {
                throw ParseFailure(proto3_extends_[i].location,
                    "extend declarations are allowed only in proto2");
            }
        }
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
            throw ParseFailure(semantic_error.location, semantic_error.message,
                               semantic_error.code);
        }
        if (defer_type_resolution_) {
            if (!easypb_schema::validate_deferred_file(out_, semantic_error)) {
                throw ParseFailure(semantic_error.location,
                                   semantic_error.message,
                                   semantic_error.code);
            }
            return;
        }
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&out_);
        if (!easypb_schema::resolve_file(out_, symbols, visible, true,
                                         semantic_error)) {
            throw ParseFailure(semantic_error.location, semantic_error.message,
                               semantic_error.code);
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
        error.code = failure.code;
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
