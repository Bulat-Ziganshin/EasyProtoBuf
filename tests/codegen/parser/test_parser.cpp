#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "proto_parser.hpp"

#include "known_options.hpp"
#include "schema_semantics.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

const FieldDescriptorProto* find_field(const DescriptorProto& message, const char* name)
{
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (message.field[i].name == name) return &message.field[i];
    }
    return 0;
}

// R09 (U-44): the removed `OptionFieldKind::repeated` flag is guarded by the
// `parser.unit.option_field_kind` CTest, not by a compile-time detector. GCC
// 4.7.4 does not SFINAE out the `decltype((void)T::repeated)` form, so the
// full-matrix job failed on a detector that reported the member of a struct
// that no longer had one (run 40600916122, job 122528516148). The guard reads
// the declaration in `codegen/parser/known_options.hpp` instead.

void test_complex_proto3()
{
    const std::string source =
        "syntax = \"proto3\";\n"
        "package demo.pkg;\n"
        "import public \"other.proto\";\n"
        "enum State { ZERO = 0; ONE = 1; }\n"
        "message Outer {\n"
        "  message Inner { string value = 1; }\n"
        "  repeated int32 nums = 1;\n"
        "  map<string, Inner> by_name = 2;\n"
        "  oneof choice { string text = 3; State state = 4; }\n"
        "}\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("complex.proto", source, parsed, error));
    CHECK(parsed.file.has_syntax && text(parsed.file.syntax) == "proto3");
    CHECK(parsed.imports.size() == 1);
    CHECK(parsed.file.has_name && text(parsed.file.name) == "complex.proto");
    CHECK(parsed.file.has_package && text(parsed.file.package) == "demo.pkg");
    CHECK(parsed.file.enum_type.size() == 1);
    CHECK(parsed.file.message_type.size() == 1);

    const DescriptorProto& outer = parsed.file.message_type[0];
    CHECK(outer.oneof_decl.size() == 1);
    CHECK(outer.nested_type.size() == 2); // Inner + synthetic map entry

    const FieldDescriptorProto* nums = find_field(outer, "nums");
    CHECK(nums != 0);
    CHECK(nums && nums->label == FieldDescriptorProto::LABEL_REPEATED);
    CHECK(nums && !nums->has_options && !nums->options.has_packed);

    const FieldDescriptorProto* by_name = find_field(outer, "by_name");
    CHECK(by_name != 0);
    CHECK(by_name && by_name->type == FieldDescriptorProto::TYPE_MESSAGE);
    CHECK(by_name && text(by_name->type_name) == ".demo.pkg.Outer.ByNameEntry");

    const FieldDescriptorProto* state = find_field(outer, "state");
    CHECK(state != 0);
    CHECK(state && state->type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(state && text(state->type_name) == ".demo.pkg.State");
    CHECK(state && state->has_oneof_index && state->oneof_index == 0);
}

void test_proto2_defaults_and_literals()
{
    const std::string source =
        "syntax='proto2';\n"
        "message M {\n"
        " optional string s = 1 [default = 'a\\n' \"b\\u263a\"];\n"
        " optional bytes b = 2 [default = \"\\x00\\377A\"];\n"
        " optional int32 i = 3 [default = -0x2a];\n"
        " optional double d = 4 [default = 1e3];\n"
        " repeated sint64 xs = 5 [packed = true];\n"
        "}\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("defaults.proto", source, parsed, error));
    CHECK(!parsed.file.has_syntax);
    const DescriptorProto& m = parsed.file.message_type[0];
    const FieldDescriptorProto* s = find_field(m, "s");
    const FieldDescriptorProto* b = find_field(m, "b");
    const FieldDescriptorProto* i = find_field(m, "i");
    const FieldDescriptorProto* d = find_field(m, "d");
    const FieldDescriptorProto* xs = find_field(m, "xs");
    CHECK(s && s->has_default_value && text(s->default_value) == std::string("a\nb\xE2\x98\xBA", 6));
    CHECK(b && b->has_default_value && text(b->default_value) == "\\000\\377A");
    CHECK(i && text(i->default_value) == "-42");
    CHECK(d && text(d->default_value) == "1000");
    CHECK(xs && xs->has_options && xs->options.has_packed && xs->options.packed);
}

void test_rejects_proto3_required()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("bad.proto",
        "syntax=\"proto3\"; message M { required int32 x = 1; }", parsed, error));
    CHECK(error.message.find("required") != std::string::npos);
}


// D-23 (contract clause 12): the SOURCE half of the field-number rule - the
// parser refuses 0, the reserved 19000..19999 range and anything above
// 536870911, while both sides of the reserved range and the maximum itself
// stay legal. These refusals had no test row before fix round r11, and they
// are what the linker's DIAGNOSTIC_INVALID_FIELD_NUMBER mirrors on
// descriptor-origin input: one rule, two frontends, the same numbers.
void test_rejects_invalid_field_numbers()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    CHECK(!easypb_proto::parse_proto("num-zero.proto",
        "message M { optional int32 x = 0; }", parsed, error));
    CHECK(error.message.find("field number must be in range 1..536870911") !=
          std::string::npos);

    CHECK(!easypb_proto::parse_proto("num-reserved.proto",
        "message M { optional int32 x = 19000; }", parsed, error));
    CHECK(error.message.find("field numbers 19000..19999 are reserved") !=
          std::string::npos);

    CHECK(!easypb_proto::parse_proto("num-reserved-high.proto",
        "message M { optional int32 x = 19999; }", parsed, error));
    CHECK(error.message.find("field numbers 19000..19999 are reserved") !=
          std::string::npos);

    CHECK(!easypb_proto::parse_proto("num-big.proto",
        "message M { optional int32 x = 536870912; }", parsed, error));
    CHECK(error.message.find("field number must be in range 1..536870911") !=
          std::string::npos);

    CHECK(easypb_proto::parse_proto("num-ok.proto",
        "message M { optional int32 a = 1; optional int32 b = 18999;"
        " optional int32 c = 20000; optional int32 d = 536870911; }",
        parsed, error));
    if (parsed.file.message_type.size() == 1) {
        const DescriptorProto& message = parsed.file.message_type[0];
        CHECK(message.field.size() == 4);
        if (message.field.size() == 4) {
            CHECK(message.field[1].number == 18999);
            CHECK(message.field[2].number == 20000);
            CHECK(message.field[3].number == 536870911);
        }
    } else {
        CHECK(parsed.file.message_type.size() == 1);
    }
}

void test_map_keyword_can_be_a_type_name()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("map-type.proto",
        "syntax=\"proto3\"; message map {} message M { map value = 1; }",
        parsed, error));
    CHECK(parsed.file.message_type.size() == 2);
    if (parsed.file.message_type.size() != 2) return;
    const FieldDescriptorProto* field = find_field(parsed.file.message_type[1], "value");
    CHECK(field != 0);
    CHECK(field && field->type == FieldDescriptorProto::TYPE_MESSAGE);
    CHECK(field && text(field->type_name) == ".map");
}

void test_rejects_invalid_map_key()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("bad-map.proto",
        "syntax=\"proto3\"; message M { map<double, string> x = 1; }", parsed, error));
    CHECK(error.message.find("map key") != std::string::npos);
}


void test_map_option_rejections_point_at_the_option()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    // U-08: an explicit `packed = false` is the option default, so the map is
    // accepted; only `true` is refused on a repeated message field.
    CHECK(easypb_proto::parse_proto("map-packed-false.proto",
        "message M { map<string, int32> x = 1 [packed = false]; }",
        parsed, error));
    // V-16: accepted is not enough - protoc keeps the explicit spelling in
    // `FieldOptions`, so the source model must carry it too. The print path
    // writes the marker only when `has_packed` is set.
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() == 1) {
        const FieldDescriptorProto* map_field =
            find_field(parsed.file.message_type[0], "x");
        CHECK(map_field != 0);
        CHECK(map_field && map_field->has_options);
        CHECK(map_field && map_field->options.has_packed);
        CHECK(map_field && !map_field->options.packed);
    }
    CHECK(!easypb_proto::parse_proto("bad-map-packed.proto",
        "message M { map<string, int32> x = 1 [packed = true]; }",
        parsed, error));
    CHECK(error.message.find("default or packed") != std::string::npos);
    CHECK(error.location.line == 1 && error.location.column == 48);
    CHECK(!easypb_proto::parse_proto("bad-map-default.proto",
        "message M { map<string, int32> x = 1 [default = 0]; }",
        parsed, error));
    CHECK(error.message.find("default or packed") != std::string::npos);
    CHECK(error.location.line == 1 && error.location.column == 49);
}

void test_rejects_wrong_default_token_kind()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("bad-default.proto",
        "message M { optional int32 x = 1 [default = \"3\"]; }", parsed, error));
    CHECK(error.message.find("integer literal") != std::string::npos);
}

void test_64_bit_default_ranges()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("wide.proto",
        "message M { optional int64 a = 1 [default = -9223372036854775808]; "
        "optional uint64 b = 2 [default = 18446744073709551615]; }", parsed, error));
    CHECK(!easypb_proto::parse_proto("too-wide.proto",
        "message M { optional uint64 b = 1 [default = 18446744073709551616]; }", parsed, error));
    CHECK(error.message.find("unsigned 64-bit") != std::string::npos);
}



void test_numeric_literal_forms()
{
    const std::string source =
        "message M {\n"
        " optional int32 oct = 1 [default = 077];\n"
        " optional uint32 hex = 2 [default = 0xffffffff];\n"
        " optional float neg_inf = 3 [default = -inf];\n"
        " optional double not_a_number = 4 [default = nan];\n"
        " optional float fraction = 5 [default = .5e+2];\n"
        " optional double one = 6 [default = 1.0];\n"
        " optional double tiny = 7 [default = 1e-8];\n"
        " optional double negative_zero = 8 [default = -0.0];\n"
        " optional float rounded = 9 [default = 1.2345678901234567];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("numbers.proto", source, parsed, error));
    const DescriptorProto& message = parsed.file.message_type[0];
    CHECK(find_field(message, "oct") && text(find_field(message, "oct")->default_value) == "63");
    CHECK(find_field(message, "hex") && text(find_field(message, "hex")->default_value) == "4294967295");
    CHECK(find_field(message, "neg_inf") && text(find_field(message, "neg_inf")->default_value) == "-inf");
    CHECK(find_field(message, "not_a_number") && text(find_field(message, "not_a_number")->default_value) == "nan");
    CHECK(find_field(message, "fraction") && text(find_field(message, "fraction")->default_value) == "50");
    CHECK(find_field(message, "one") && text(find_field(message, "one")->default_value) == "1");
    CHECK(find_field(message, "tiny") && text(find_field(message, "tiny")->default_value) == "1e-08");
    CHECK(find_field(message, "negative_zero") && text(find_field(message, "negative_zero")->default_value) == "-0");
    CHECK(find_field(message, "rounded") && text(find_field(message, "rounded")->default_value) == "1.23456788");
}

// R1's source half: the parser stores the canonical text the descriptor path
// stores, for the whole numeric-default spelling family. CR-01 (an INTEGER
// token on a float/double is octal/hex-aware, never decimal text), CR-17
// (integral defaults become decimal) and CR-05's 64-bit extremes (the stored
// text is canonical; the emitter owns the typed C++ literal) are asserted on
// the same schema so a regression in one path is visible in one place.
void test_numeric_default_spellings_are_canonical()
{
    const std::string source =
        "syntax = \"proto2\";\n"
        "message M {\n"
        " optional float f_octal = 1 [default = 010];\n"
        " optional double d_octal = 2 [default = 010];\n"
        " optional float f_hex = 3 [default = 0x10];\n"
        " optional double d_hex = 4 [default = 0x10];\n"
        " optional float f_neg_octal = 5 [default = -010];\n"
        " optional int32 i32_octal = 6 [default = 010];\n"
        " optional int32 i32_hex = 7 [default = 0x10];\n"
        " optional int32 i32_neg_zero = 8 [default = -0];\n"
        " optional int64 i64_octal = 9 [default = 077];\n"
        " optional int64 i64_min = 10 [default = -9223372036854775808];\n"
        " optional int64 i64_max = 11 [default = 9223372036854775807];\n"
        " optional uint64 u64_max = 12 [default = 18446744073709551615];\n"
        " optional uint64 u64_hex_max = 13 [default = 0xFFFFFFFFFFFFFFFF];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("spellings.proto", source, parsed, error));
    const DescriptorProto& message = parsed.file.message_type[0];
    struct Row {
        const char* name;
        const char* expected;
    };
    const Row rows[] = {
        {"f_octal", "8"},
        {"d_octal", "8"},
        {"f_hex", "16"},
        {"d_hex", "16"},
        {"f_neg_octal", "-8"},
        {"i32_octal", "8"},
        {"i32_hex", "16"},
        {"i32_neg_zero", "0"},
        {"i64_octal", "63"},
        {"i64_min", "-9223372036854775808"},
        {"i64_max", "9223372036854775807"},
        {"u64_max", "18446744073709551615"},
        {"u64_hex_max", "18446744073709551615"}
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const FieldDescriptorProto* field = find_field(message, rows[i].name);
        CHECK(field != 0);
        if (field != 0) {
            CHECK(field->has_default_value);
            CHECK(text(field->default_value) == rows[i].expected);
        }
    }

    // CR-18's overlap: the integer token `08` is not octal, and the lexer
    // owns that rule now (B4). The refusal happens while tokenizing, so no
    // consumer can turn the typo into a value: the float path never sees a
    // decimal 8 either.
    CHECK(!easypb_proto::parse_proto("bad-octal-float.proto",
        "message M { optional float x = 1 [default = 08]; }", parsed, error));
    CHECK(error.message.find("leading zero must be octal") != std::string::npos);
}

void test_lexer_diagnostics()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    // B4: the tokenizer refuses `08` before the integral-default range check
    // can see it, so the diagnostic names the octal rule on every consumer.
    CHECK(!easypb_proto::parse_proto("bad-octal.proto",
        "message M { optional int32 x = 1 [default = 08]; }", parsed, error));
    CHECK(error.message.find("leading zero must be octal") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("bad-exponent.proto",
        "message M { optional double x = 1 [default = 1e]; }", parsed, error));
    CHECK(error.message.find("exponent") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("bad-escape.proto",
        "message M { optional string x = 1 [default = \"\\x\"]; }", parsed, error));
    CHECK(error.message.find("hexadecimal") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("bad-comment.proto", "/* unterminated", parsed, error));
    CHECK(error.message.find("unterminated block comment") != std::string::npos);
    CHECK(error.location.line == 1 && error.location.column == 1);
}

// U-33: protoc's `Tokenizer::NextChar` advances a tab to the next multiple of
// `kTabWidth = 8` (`io/tokenizer.cc:246-256`), so a diagnostic after a tab
// names the same column the reference compiler would. A tab that starts at
// column 1, 2 or 8 lands on column 9; a tab at column 9 lands on 17.
void test_tab_columns_expand_to_tab_stops()
{
    struct Case
    {
        const char* name;
        std::size_t spaces;
        std::size_t column;
    };
    static const Case cases[] = {
        {"tab-column-1.proto", 0, 9},
        {"tab-column-2.proto", 1, 9},
        {"tab-column-8.proto", 7, 9},
        {"tab-column-9.proto", 8, 17}
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::string source = "syntax = \"proto3\";\n";
        source.append(cases[i].spaces, ' ');
        source += "\t!\n";
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto(cases[i].name, source, parsed, error));
        CHECK(error.message.find("unexpected character") != std::string::npos);
        CHECK(error.location.line == 2);
        CHECK(error.location.column == cases[i].column);
    }
}

// R2 / B4: the escape table, one row per protoc rule
// (`Tokenizer::ParseStringAppend`, `FetchUnicodePoint`, `AppendUTF8`;
// `io/tokenizer.cc:1049-1235`). A string default stores the decoded bytes; a
// bytes default stores the C-escaped text, exactly as the descriptor path
// does, so both columns are asserted. CR-12 is the low-byte octal, CR-13 the
// surrogate pair and the lone surrogate, CR-29 the raw CR.
void test_lexer_escape_table()
{
    const std::string source =
        "syntax = \"proto2\";\n"
        "message M {\n"
        " optional string s_hex = 1 [default = \"\\x41\"];\n"
        " optional string s_hex_upper = 2 [default = \"\\X41\"];\n"
        " optional string s_hex_one = 3 [default = \"\\x9\"];\n"
        " optional string s_octal = 4 [default = \"\\101\"];\n"
        " optional string s_max = 5 [default = \"\\777\"];\n"
        " optional string s_nul = 6 [default = \"a\\400b\"];\n"
        " optional string s_one = 7 [default = \"\\0\"];\n"
        " optional string s_u2 = 8 [default = \"\\u00e9\"];\n"
        " optional string s_u3 = 9 [default = \"\\u20ac\"];\n"
        " optional string s_u4 = 10 [default = \"\\U0001F600\"];\n"
        " optional string s_pair = 11 [default = \"\\ud83d\\ude00\"];\n"
        " optional string s_head = 12 [default = \"\\ud800\"];\n"
        " optional string s_tail = 13 [default = \"\\U0000DC00\"];\n"
        " optional string s_over = 14 [default = \"\\U00110000\"];\n"
        " optional string s_head_ascii = 15 [default = \"\\ud800\\u0041\"];\n"
        " optional string s_cr = 16 [default = \"x\ry\"];\n"
        " optional bytes  b_max = 17 [default = \"\\777\"];\n"
        " optional bytes  b_pair = 18 [default = \"\\ud83d\\ude00\"];\n"
        " optional bytes  b_over = 19 [default = \"\\U00110000\"];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("escapes.proto", source, parsed, error));
    const DescriptorProto& message = parsed.file.message_type[0];
    struct Row {
        const char* name;
        const char* expected;
        std::size_t size;
    };
    const std::string astral("\xf0\x9f\x98\x80", 4);
    const std::string lone_head("\xed\xa0\x80", 3);
    const std::string lone_tail("\xed\xb0\x80", 3);
    const std::string head_ascii = lone_head + "A";
    const std::string nul_middle("a\0b", 3);
    const std::string nul_only("\0", 1);
    const Row rows[] = {
        {"s_hex", "A", 1},
        {"s_hex_upper", "A", 1},
        {"s_hex_one", "\t", 1},
        {"s_octal", "A", 1},
        {"s_max", "\xff", 1},
        {"s_nul", nul_middle.data(), nul_middle.size()},
        {"s_one", nul_only.data(), nul_only.size()},
        {"s_u2", "\xc3\xa9", 2},
        {"s_u3", "\xe2\x82\xac", 3},
        {"s_u4", astral.data(), astral.size()},
        {"s_pair", astral.data(), astral.size()},
        {"s_head", lone_head.data(), lone_head.size()},
        {"s_tail", lone_tail.data(), lone_tail.size()},
        // Above 0x10FFFF protoc appends the escape text itself.
        {"s_over", "\\U00110000", 10},
        {"s_head_ascii", head_ascii.data(), head_ascii.size()},
        {"s_cr", "x\ry", 3},
        // A bytes default is stored C-escaped, like the descriptor path.
        {"b_max", "\\377", 4},
        {"b_pair", "\\360\\237\\230\\200", 16},
        {"b_over", "\\\\U00110000", 11},
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const FieldDescriptorProto* field = find_field(message, rows[i].name);
        CHECK(field != 0);
        if (field != 0) {
            CHECK(field->has_default_value);
            CHECK(text(field->default_value) ==
                  std::string(rows[i].expected, rows[i].size));
        }
    }

    // The refusals. `\U` is bounded to `00[01]xxxxxx` by `ConsumeString`
    // (`io/tokenizer.cc:404-421`), a raw LF ends the literal, and a raw NUL
    // is end-of-string, not content.
    struct Reject {
        const char* name;
        const char* body;
        const char* message;
    };
    const Reject rejects[] = {
        {"over-shape.proto", "message M { optional string s = 1 [default = \"\\U00FFFFFF\"]; }",
         "Unicode escape"},
        {"raw-lf.proto", "message M { optional string s = 1 [default = \"x\ny\"]; }",
         "newline in string literal"},
        {"raw-crlf.proto", "message M { optional string s = 1 [default = \"x\r\ny\"]; }",
         "newline in string literal"},
        {"short-u.proto", "message M { optional string s = 1 [default = \"\\u12\"]; }",
         "Unicode escape"},
        {"bad-escape.proto", "message M { optional string s = 1 [default = \"\\e\"]; }",
         "escape"},
    };
    for (std::size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); ++i) {
        CHECK(!easypb_proto::parse_proto(rejects[i].name, rejects[i].body,
                                         parsed, error));
        CHECK(error.message.find(rejects[i].message) != std::string::npos);
    }
}

// R2 / B4: the lexeme table of `Tokenizer::ConsumeNumber`
// (`io/tokenizer.cc:440-495`). A leading zero makes the token octal (or hex
// with `0x`); an octal token has no 8/9 digit, no fraction and no exponent.
// The rule lives in the tokenizer, so every consumer refuses the typo.
void test_lexer_lexeme_table()
{
    const std::string accepted =
        "syntax = \"proto2\";\n"
        "message M {\n"
        " optional int32  i_hex = 1 [default = 0x10];\n"
        " optional int32  i_octal = 2 [default = 010];\n"
        " optional int32  i_max = 3 [default = 077];\n"
        " optional int32  i_double_zero = 4 [default = 00];\n"
        " optional int32  i_zero = 5 [default = 0];\n"
        " optional float  f_octal = 6 [default = 010];\n"
        " optional double d_fraction = 7 [default = 0.5];\n"
        " optional double d_exp = 8 [default = 0e5];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("lexemes.proto", accepted, parsed, error));
    const DescriptorProto& message = parsed.file.message_type[0];
    const char* names[] = {"i_hex", "i_octal", "i_max", "i_double_zero",
                           "i_zero", "f_octal", "d_fraction", "d_exp"};
    const char* values[] = {"16", "8", "63", "0", "0", "8", "0.5", "0"};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const FieldDescriptorProto* field = find_field(message, names[i]);
        CHECK(field != 0);
        if (field != 0) {
            CHECK(field->has_default_value);
            CHECK(text(field->default_value) == values[i]);
        }
    }

    struct Reject {
        const char* name;
        const char* body;
        const char* message;
    };
    const Reject rejects[] = {
        {"int-08.proto", "message M { optional int32 a = 1 [default = 08]; }",
         "leading zero must be octal"},
        {"int-09.proto", "message M { optional int32 a = 1 [default = 09]; }",
         "leading zero must be octal"},
        {"float-08.proto", "message M { optional float a = 1 [default = 08]; }",
         "leading zero must be octal"},
        {"option-08.proto", "option (my.opt) = 08;", "leading zero must be octal"},
        {"option-09.proto", "option (my.opt) = 09;", "leading zero must be octal"},
        {"enum-08.proto", "enum E { A = 0; B = 08; }", "leading zero must be octal"},
        {"octal-fraction.proto", "message M { optional double a = 1 [default = 010.5]; }",
         "hex and octal numbers must be integers"},
        {"hex-fraction.proto", "message M { optional double a = 1 [default = 0x10.5]; }",
         "hex and octal numbers must be integers"},
        {"octal-exponent.proto", "message M { optional double a = 1 [default = 010e5]; }",
         "invalid suffix"},
    };
    for (std::size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); ++i) {
        CHECK(!easypb_proto::parse_proto(rejects[i].name, rejects[i].body,
                                         parsed, error));
        CHECK(error.message.find(rejects[i].message) != std::string::npos);
    }
}

// R2 / B4: CR-30 (block comments do not nest) and CR-19 (a raw NUL is a
// lexical error, never a swallowed token). A `/*` inside a line comment and an
// empty block comment stay legal; protoc only refuses the nested opener in a
// block comment (`io/tokenizer.cc:516-550`).
void test_lexer_comment_and_control_rules()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    CHECK(!easypb_proto::parse_proto("nested.proto",
        "/* outer /* inner */\nsyntax = \"proto3\";\nmessage M { int32 a = 1; }\n",
        parsed, error));
    CHECK(error.message.find("cannot be nested") != std::string::npos);

    CHECK(easypb_proto::parse_proto("line-star.proto",
        "// a /* b\nsyntax = \"proto3\";\nmessage M { int32 a = 1; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("empty-comment.proto",
        "/**/\nsyntax = \"proto3\";\nmessage M { int32 a = 1; }\n",
        parsed, error));

    // U-17: a line comment ends only at LF or end of input, so a bare CR is
    // content. `message Hidden` stays inside the comment; a lexer that stopped
    // at the CR would parse it as a second message. The garbage line proves
    // the accepted verdict of protoc's `ConsumeLineComment`, and the CRLF
    // control keeps its terminator and line numbering.
    CHECK(easypb_proto::parse_proto("line-cr.proto",
        "syntax = \"proto2\";\n"
        "// c\rmessage Hidden { optional int32 a = 1; }\n"
        "message M { optional int32 b = 1; }\n", parsed, error));
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() == 1) {
        CHECK(text(parsed.file.message_type[0].name) == "M");
    }
    CHECK(easypb_proto::parse_proto("line-cr-garbage.proto",
        "syntax = \"proto2\";\n// c\rgarbage garbage garbage\n", parsed, error));
    CHECK(easypb_proto::parse_proto("line-crlf.proto",
        "syntax = \"proto2\";\n// c\r\nmessage M { optional int32 b = 1; }\n",
        parsed, error));
    CHECK(parsed.file.message_type.size() == 1);
    // Only LF starts a line: the error after the CR-bearing comment is on the
    // third physical line, not the fourth a CR-as-newline counter would name.
    CHECK(!easypb_proto::parse_proto("line-cr-position.proto",
        "syntax = \"proto2\";\n// c\rstill comment\n!\n", parsed, error));
    CHECK(error.location.line == 3);
    CHECK(error.location.column == 1);

    // The aggregate scanner is the consumer CR-19 measured: the NUL used to
    // match `strchr(punctuation, '\0')` and disappear as a symbol token.
    std::string aggregate =
        "syntax = \"proto2\";\nmessage M { optional int32 a = 1; }\n"
        "option (my.opt) = { a: 1";
    aggregate.push_back('\0');
    aggregate += " };\n";
    CHECK(!easypb_proto::parse_proto("aggregate-nul.proto", aggregate, parsed, error));
    CHECK(error.message.find("control character") != std::string::npos);
    // The byte is escaped in the sentence, so no control byte reaches a
    // diagnostic (P9).
    CHECK(error.message.find("0x00") != std::string::npos);

    // The same rule outside an aggregate: a top-level NUL is not whitespace.
    std::string top_level = "syntax = \"proto3\";\n";
    top_level.push_back('\0');
    CHECK(!easypb_proto::parse_proto("top-nul.proto", top_level, parsed, error));
    CHECK(error.message.find("control character") != std::string::npos);
}

// A25's source half (plan 5, ledger D-44): `edition = "2023";` names an input
// capability this project does not model, so the statement dispatcher refuses
// it by name instead of falling through to the generic top-level message. The
// unknown-`syntax` path keeps its own message, and neither refusal weakens the
// other; proto2 and proto3 statements stay accepted.
void test_edition_statement_is_refused_by_name()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("editions.proto",
        "edition = \"2023\";\npackage s;\nmessage M { int32 a = 1; }\n",
        parsed, error));
    CHECK(error.message.find("editions syntax is not supported") != std::string::npos);
    CHECK(error.message.find("proto2") != std::string::npos &&
          error.message.find("proto3") != std::string::npos);
    CHECK(error.message.find("expected a top-level") == std::string::npos);
    CHECK(error.location.line == 1 && error.location.column == 1);
    // S7e (W-60): the source statement carries the descriptor twin's code.
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);

    // The unknown-syntax path is unchanged (A19): the literal is what fails,
    // not the statement shape, and the message is its own. The position is the
    // value token, as protoc records it (W-65), and the code is the twin's.
    CHECK(!easypb_proto::parse_proto("proto4.proto",
        "syntax = \"proto4\";\npackage s;\n", parsed, error));
    CHECK(error.message == "syntax must be \"proto2\" or \"proto3\"");
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
    CHECK(error.location.line == 1 && error.location.column == 10);

    // Both supported statements still parse.
    CHECK(easypb_proto::parse_proto("p2.proto",
        "syntax = \"proto2\";\nmessage M { optional int32 a = 1; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("p3.proto",
        "syntax = \"proto3\";\nmessage M { int32 a = 1; }\n", parsed, error));
}

void test_rejects_reserved_conflicts()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("reserved-number.proto",
        "message M { reserved 2, 4 to 6; optional int32 x = 2; }", parsed, error));
    CHECK(error.message.find("reserved") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("reserved-name.proto",
        "message M { reserved \"old\"; optional int32 old = 1; }", parsed, error));
    CHECK(error.message.find("reserved") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("reserved-range.proto",
        "message M { reserved 10 to 5; }", parsed, error));
    CHECK(error.message.find("range") != std::string::npos);
}

void test_rejects_extension_range_conflicts()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("extension-field.proto",
        "message M { extensions 100 to 199; optional int32 x = 150; }", parsed, error));
    CHECK(error.message.find("extension range") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("extension-reserved.proto",
        "message M { extensions 100 to 199; reserved 150 to 250; }", parsed, error));
    CHECK(error.message.find("overlap") != std::string::npos);
}

// V-15: `reserved` and `extensions` are descriptor content, not parser-local
// text. The source print reads the same tree as the `.pbs` print, so the model
// keeps the source order, the inclusive enum end and the exclusive message end
// (one past the last source number). A `to max` endpoint is resolved before it
// is stored, because its value depends on `message_set_wire_format`.
void test_reserved_and_extension_ranges_are_retained()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("ranges.proto",
        "syntax=\"proto2\";\n"
        "message M {\n"
        "  optional int32 a = 1;\n"
        "  reserved 5 to 10;\n"
        "  reserved \"zeta\", \"alpha\";\n"
        "  extensions 100 to 199;\n"
        "  enum E { ZERO = 0; reserved 2 to 3; reserved \"B\"; }\n"
        "}\n",
        parsed, error));
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() != 1) return;
    const DescriptorProto& message = parsed.file.message_type[0];

    CHECK(message.reserved_range.size() == 1);
    if (message.reserved_range.size() == 1) {
        CHECK(message.reserved_range[0].has_start);
        CHECK(message.reserved_range[0].has_end);
        CHECK(message.reserved_range[0].start == 5);
        CHECK(message.reserved_range[0].end == 11); // inclusive 10 -> exclusive 11
    }
    // The de-duplicating set sorts; the model must keep the declaration order.
    CHECK(message.reserved_name.size() == 2);
    if (message.reserved_name.size() == 2) {
        CHECK(text(message.reserved_name[0]) == "zeta");
        CHECK(text(message.reserved_name[1]) == "alpha");
    }
    CHECK(message.extension_range.size() == 1);
    if (message.extension_range.size() == 1) {
        CHECK(message.extension_range[0].has_start);
        CHECK(message.extension_range[0].has_end);
        CHECK(message.extension_range[0].start == 100);
        CHECK(message.extension_range[0].end == 200);
    }

    CHECK(message.enum_type.size() == 1);
    if (message.enum_type.size() == 1) {
        const EnumDescriptorProto& enumeration = message.enum_type[0];
        CHECK(enumeration.reserved_range.size() == 1);
        if (enumeration.reserved_range.size() == 1) {
            CHECK(enumeration.reserved_range[0].start == 2);
            CHECK(enumeration.reserved_range[0].end == 3); // enum end is inclusive
        }
        CHECK(enumeration.reserved_name.size() == 1);
        if (enumeration.reserved_name.size() == 1) {
            CHECK(text(enumeration.reserved_name[0]) == "B");
        }
    }

    CHECK(easypb_proto::parse_proto("range-max.proto",
        "syntax=\"proto2\"; message M { extensions 1 to max; }", parsed, error));
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() == 1 &&
        parsed.file.message_type[0].extension_range.size() == 1) {
        CHECK(parsed.file.message_type[0].extension_range[0].end == 536870912);
    }
    CHECK(easypb_proto::parse_proto("range-max-set.proto",
        "syntax=\"proto2\"; message M { option message_set_wire_format = true;"
        " extensions 1 to max; }", parsed, error));
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() == 1 &&
        parsed.file.message_type[0].extension_range.size() == 1) {
        CHECK(parsed.file.message_type[0].extension_range[0].end == 2147483647);
    }
}

// V-03: a reserved declaration's endpoints may reach INT32_MAX - 1. protoc
// stores the range as [start, end) and asks only that the start is positive
// and below the exclusive end (descriptor.cc:6906-6922), so 2147483646 is the
// largest spellable endpoint ("2147483647" overflows the exclusive end). The
// descriptor door already accepted this bound; the source parser capped it at
// 536870912.
void test_reserved_range_reaches_the_int32_bound()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    CHECK(easypb_proto::parse_proto("reserved-wide-single.proto",
        "syntax=\"proto2\"; message M { reserved 536870913; }", parsed, error));
    CHECK(easypb_proto::parse_proto("reserved-wide-range.proto",
        "syntax=\"proto2\"; message M { reserved 1 to 2147483646; }", parsed, error));
    CHECK(easypb_proto::parse_proto("reserved-wide-open.proto",
        "syntax=\"proto2\"; message M { reserved 536870912 to 2147483646; }",
        parsed, error));

    CHECK(!easypb_proto::parse_proto("reserved-over-int32.proto",
        "syntax=\"proto2\"; message M { reserved 2147483647; }", parsed, error));
    CHECK(error.message.find("reserved range endpoint must be in range "
                             "1..2147483646") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("reserved-zero-wide.proto",
        "syntax=\"proto2\"; message M { reserved 0; }", parsed, error));
    CHECK(error.message.find("reserved range endpoint must be in range "
                             "1..2147483646") != std::string::npos);
}

// V-05: a MessageSet's extension ranges and tags are plain int32 numbers, so
// `to max` expands to 2147483646 and an explicit endpoint may reach it
// (protoc's `AdjustExtensionRangesWithMaxEndNumber`, parser.cc:827-837, and
// `ValidateExtensionRangeOptions`, descriptor.cc:8385-8404). An ordinary
// message keeps the 536870911 cap. The option may follow the range it widens,
// which is why the expansion is a post-body rule.
void test_message_set_extension_bounds()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    CHECK(easypb_proto::parse_proto("messageset-max.proto",
        "syntax=\"proto2\";\n"
        "message M { extensions 100 to max;"
        " option message_set_wire_format = true; }\n"
        "extend M { optional M ext = 600000000; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("messageset-wide.proto",
        "syntax=\"proto2\";\n"
        "message M { option message_set_wire_format = true;"
        " extensions 600000000 to 600000001; }\n",
        parsed, error));

    // The ordinary message keeps both caps: an explicit endpoint above
    // 536870911 is refused after the body, where the option is known.
    CHECK(!easypb_proto::parse_proto("ordinary-wide-range.proto",
        "syntax=\"proto2\"; message M { extensions 1 to 599999999; }",
        parsed, error));
    CHECK(error.message.find("field number must be in range 1..536870911") !=
          std::string::npos);
    CHECK(!easypb_proto::parse_proto("ordinary-wide-single.proto",
        "syntax=\"proto2\"; message M { extensions 600000000; }",
        parsed, error));
    CHECK(error.message.find("field number must be in range 1..536870911") !=
          std::string::npos);

    // A MessageSet's range may still be refused for its shape.
    CHECK(!easypb_proto::parse_proto("messageset-inverted.proto",
        "syntax=\"proto2\";\n"
        "message M { option message_set_wire_format = true;"
        " extensions 600000001 to 600000000; }\n",
        parsed, error));
    CHECK(error.message.find("extension range end is smaller") !=
          std::string::npos);
}

void test_rejects_reserved_enum_values()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("reserved-enum.proto",
        "enum E { reserved -2 to -1, 7; reserved \"OLD\"; ZERO = 0; OLD = 1; }",
        parsed, error));
    CHECK(error.message.find("reserved") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("reserved-enum-after.proto",
        "enum E { OLD = 1; reserved \"OLD\"; }", parsed, error));
    CHECK(error.message.find("reserved") != std::string::npos);
}

void test_rejects_enum_scope_name_collisions()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const char* collisions[] = {
        "enum A { VALUE = 0; } enum B { VALUE = 0; }",
        "message M { enum A { VALUE = 0; } enum B { VALUE = 0; } }",
        "message M { enum E { X = 0; } optional int32 X = 1; }",
        "message M { enum E { X = 0; } message X {} }",
        "message M { enum E { X = 0; } enum X { Y = 0; } }",
        "message M { enum E { X = 0; } oneof X { int32 a = 1; } }",
        "enum E { X = 0; } message X {}",
        "enum E { X = 0; } enum X { Y = 0; }"
    };

    for (std::size_t i = 0; i < sizeof(collisions) / sizeof(collisions[0]); ++i) {
        CHECK(!easypb_proto::parse_proto(
            "enum-scope-collision.proto", collisions[i], parsed, error));
        CHECK(error.message.find("name") != std::string::npos);
    }

    CHECK(easypb_proto::parse_proto("separate-enum-scopes.proto",
        "message A { enum E { VALUE = 0; } } "
        "message B { enum E { VALUE = 0; } }",
        parsed, error));
}

void test_rejects_invalid_enum_defaults()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("invalid-enum-default.proto",
        "enum E { A = 0; } "
        "message M { optional E e = 1 [default = B]; }",
        parsed, error));
    CHECK(error.message.find("no value named B") != std::string::npos);
}

// V-04: protoc's enum-default branch accepts any identifier token
// (`ParseDefaultAssignment`, parser.cc:1330-1340), and its tokenizer has no
// separate true/false/inf/nan kind, so `enum E { true = 0; }` with
// `[default = true]` is legal and the descriptor door already emits
// `::E::true_`. EasyPB's Constant classifies the four literals, so the parser
// has to admit exactly those shapes and keep the identifier rule for every
// other constant kind.
void test_keyword_enum_defaults()
{
    const std::string source =
        "syntax=\"proto2\";\n"
        "enum E { true = 0; false = 1; inf = 2; nan = 3; }\n"
        "message M {\n"
        "  optional E a = 1 [default = true];\n"
        "  optional E b = 2 [default = false];\n"
        "  optional E c = 3 [default = inf];\n"
        "  optional E d = 4 [default = nan];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("keyword-enum-default.proto", source,
                                   parsed, error));
    if (parsed.file.message_type.size() == 1) {
        const DescriptorProto& message = parsed.file.message_type[0];
        CHECK(message.field.size() == 4);
        if (message.field.size() == 4) {
            CHECK(text(message.field[0].default_value) == "true");
            CHECK(text(message.field[1].default_value) == "false");
            CHECK(text(message.field[2].default_value) == "inf");
            CHECK(text(message.field[3].default_value) == "nan");
        }
    } else {
        CHECK(parsed.file.message_type.size() == 1);
    }

    // The identifier rule is not weakened: a numeric float, a signed literal
    // and a string stay refused on an enum field.
    CHECK(!easypb_proto::parse_proto("enum-default-float.proto",
        "syntax=\"proto2\"; enum E { A = 0; } "
        "message M { optional E e = 1 [default = 1.5]; }",
        parsed, error));
    CHECK(error.message.find("enum default must be an identifier") !=
          std::string::npos);
    CHECK(!easypb_proto::parse_proto("enum-default-signed.proto",
        "syntax=\"proto2\"; enum E { A = 0; } "
        "message M { optional E e = 1 [default = -inf]; }",
        parsed, error));
    CHECK(error.message.find("enum default must be an identifier") !=
          std::string::npos);
    CHECK(!easypb_proto::parse_proto("enum-default-string.proto",
        "syntax=\"proto2\"; enum E { A = 0; } "
        "message M { optional E e = 1 [default = \"A\"]; }",
        parsed, error));
    CHECK(error.message.find("enum default must be an identifier") !=
          std::string::npos);
}

// V-08: protoc's oneof body has no EmptyStatement production; a bare `;` is
// the start of a field and fails as `Expected type name.`
// (parser.cc:2022-2085).
void test_oneof_rejects_empty_statement()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("oneof-semicolon.proto",
        "syntax=\"proto2\"; message M { oneof o { ; string a = 1; } }",
        parsed, error));
    CHECK(error.message.find("Expected type name.") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("oneof-semicolon-after.proto",
        "syntax=\"proto2\"; message M { oneof o { string a = 1; ; } }",
        parsed, error));
    CHECK(error.message.find("Expected type name.") != std::string::npos);
}

// D-01 / V-17: protoc's `ParseExtend` is a do-while that parses the first
// token as a field (parser.cc:1979-2010), so an empty or semicolon-only body
// fails with `Expected "required", "optional", or "repeated".`
// (parser.cc:996-997). The E6 warning describes only non-empty valid blocks.
void test_extend_requires_a_field()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const char* const expected =
        "Expected \"required\", \"optional\", or \"repeated\".";

    CHECK(!easypb_proto::parse_proto("extend-empty.proto",
        "syntax=\"proto2\"; message M { extensions 100 to 200; } "
        "extend M { }", parsed, error));
    CHECK(error.message.find(expected) != std::string::npos);
    CHECK(!easypb_proto::parse_proto("extend-semicolon.proto",
        "syntax=\"proto2\"; message M { extensions 100 to 200; } "
        "extend M { ; }", parsed, error));
    CHECK(error.message.find(expected) != std::string::npos);

    CHECK(easypb_proto::parse_proto("extend-one-field.proto",
        "syntax=\"proto2\"; message M { extensions 100 to 200; } "
        "extend M { optional int32 e = 100; }", parsed, error));
    bool saw_extend_warning = false;
    for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
        if (parsed.warnings[i].message.find(
                "extend declaration parsed but not stored") !=
            std::string::npos) {
            saw_extend_warning = true;
        }
    }
    CHECK(saw_extend_warning);
}

// V-12: protoc compares the *resolved* extendee name with the proto3
// allowlist (`ValidateProto3Field`, descriptor.cc:7907-7911), so a relative
// spelling in `package google.protobuf` names an allowed extendee. The package
// may be declared after the `extend` statement, which is why the verdict is
// decided at the end of the file.
void test_proto3_extendee_is_resolved_before_the_allowlist()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    CHECK(easypb_proto::parse_proto("proto3-package-extend.proto",
        "syntax=\"proto3\";\n"
        "package google.protobuf;\n"
        "import \"descriptor-stub.proto\";\n"
        "message Holder { int32 x = 1; }\n"
        "extend FileOptions { optional int32 local = 50000; }\n",
        parsed, error));
    CHECK(parsed.file.message_type.size() == 1);

    // The package may follow the statement it qualifies.
    CHECK(easypb_proto::parse_proto("proto3-package-after.proto",
        "syntax=\"proto3\";\n"
        "import \"descriptor-stub.proto\";\n"
        "extend FileOptions { optional int32 local = 50000; }\n"
        "package google.protobuf;\n",
        parsed, error));

    // Absolute spellings and the global-scope fallback keep working from a
    // different package.
    CHECK(easypb_proto::parse_proto("proto3-absolute-extend.proto",
        "syntax=\"proto3\";\n"
        "package local;\n"
        "import \"descriptor-stub.proto\";\n"
        "extend .google.protobuf.FileOptions { optional int32 local = 50000; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("proto3-global-spelling.proto",
        "syntax=\"proto3\";\n"
        "package local;\n"
        "import \"descriptor-stub.proto\";\n"
        "extend google.protobuf.FileOptions { optional int32 local = 50000; }\n",
        parsed, error));

    // A name that resolves outside the nine descriptor-option messages keeps
    // the proto2 refusal.
    CHECK(!easypb_proto::parse_proto("proto3-user-extend.proto",
        "syntax=\"proto3\";\n"
        "package local;\n"
        "import \"descriptor-stub.proto\";\n"
        "extend FileOptions { optional int32 local = 50000; }\n",
        parsed, error));
    CHECK(error.message.find("extend declarations are allowed only in proto2") !=
          std::string::npos);
}

// D-32 (fix round r16): the source half of the empty-enum rule. The parser
// refuses the shape as text, which is why the descriptor-origin half is what
// the linker owns. A body that writes `allow_alias = true` and no value is
// refused by protoc's parser first ("declares support for enum aliases but no
// enum values share field numbers", `ValidateEnum`, parser.cc:561-572), so the
// source verdict is the same refusal for the option's own reason; the empty
// descriptor-origin enum keeps the linker's empty-value rule.
void test_rejects_empty_enums()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto(
        "empty-enum.proto", "syntax=\"proto2\"; enum E {}", parsed, error));
    CHECK(error.message.find("at least one value") != std::string::npos);

    easypb_proto::ParsedProto option_only;
    easypb_proto::Diagnostic option_error;
    CHECK(!easypb_proto::parse_proto("empty-enum-alias.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = true; }",
        option_only, option_error));
    CHECK(option_error.message.find("no enum values share field numbers") !=
          std::string::npos);
}

void test_post_body_refusals_point_inside_the_declaration()
{
    // CR-42: the rules below are answered after the closing brace has been
    // consumed, so `current_` used to name the token after the declaration
    // (a duplicate enum number in a four-line message was reported at 5:1).
    // The table is the position snapshot: the empty-enum rule points at the
    // enum name, the two value rules at the value's number (protoc's own
    // columns), and the field rules at the field name. Every position is
    // inside the declaration it is about.
    struct Case
    {
        const char* name;
        const char* source;
        std::size_t line;
        std::size_t column;
        const char* fragment;
    };
    static const Case cases[] = {
        {"empty-enum-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  enum E {\n"
         "  }\n"
         "}\n",
         3, 8, "at least one value"},
        {"first-enum-value-position.proto",
         "syntax=\"proto3\";\n"
         "message M {\n"
         "  enum E {\n"
         "    A = 1;\n"
         "  }\n"
         "}\n",
         4, 9, "first proto3 enum value must be zero"},
        {"duplicate-enum-number-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  enum E { A = 0; B = 0; }\n"
         "}\n",
         3, 23, "duplicate enum number"},
        {"reserved-field-number-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  reserved 5;\n"
         "  optional int32 x = 5;\n"
         "}\n",
         4, 18, "field number is reserved"},
        {"reserved-field-name-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  reserved \"x\";\n"
         "  optional int32 x = 7;\n"
         "}\n",
         4, 18, "field name is reserved"},
        {"extension-range-field-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  extensions 100 to 200;\n"
         "  optional int32 x = 150;\n"
         "}\n",
         4, 18, "inside an extension range"},
        {"reserved-extension-overlap-position.proto",
         "syntax=\"proto2\";\n"
         "message M {\n"
         "  extensions 100 to 199;\n"
         "  reserved 150 to 250;\n"
         "}\n",
         3, 14, "overlap"},
        // U-32: the reserved declaration follows the value that reuses it, so
        // the post-body re-check is the one that fires. It must point at the
        // value (2:17), not at the token after the closing brace.
        {"reserved-enum-number-after-position.proto",
         "syntax=\"proto2\";\n"
         "enum E { A = 0; B = 2; reserved 2; }\n",
         2, 17, "enum value number is reserved"},
        {"reserved-enum-name-after-position.proto",
         "syntax=\"proto2\";\n"
         "enum E { A = 0; OLD = 1; reserved \"OLD\"; }\n",
         2, 17, "enum value name is reserved"}
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto(cases[i].name, cases[i].source,
                                        parsed, error));
        CHECK(error.message.find(cases[i].fragment) != std::string::npos);
        CHECK(error.location.line == cases[i].line);
        CHECK(error.location.column == cases[i].column);
    }
}

void test_source_refusals_carry_their_descriptor_codes()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    // W-60: each grammar refusal that has a descriptor twin names that twin's
    // code, so the two input paths agree on the code as well as on the
    // verdict. The accept halves are the proto2 controls of the same
    // declarations (and the parity corpus's `s-*-accept` rows).
    CHECK(!easypb_proto::parse_proto("proto3-required.proto",
        "syntax = \"proto3\"; message M { required int32 a = 1; }",
        parsed, error));
    CHECK(error.message.find("required") != std::string::npos);
    CHECK(error.code == easypb_schema::DIAGNOSTIC_PROTO3_REQUIRED_FIELD);

    CHECK(!easypb_proto::parse_proto("proto3-default.proto",
        "syntax = \"proto3\"; message M { int32 a = 1 [default = 5]; }",
        parsed, error));
    CHECK(error.message.find("explicit default") != std::string::npos);
    CHECK(error.code == easypb_schema::DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT);

    CHECK(!easypb_proto::parse_proto("duplicate-enum.proto",
        "syntax = \"proto2\"; enum E { A = 1; B = 1; }", parsed, error));
    CHECK(error.message.find("allow_alias") != std::string::npos);
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);

    CHECK(!easypb_proto::parse_proto("empty-enum.proto",
        "syntax = \"proto2\"; enum E {}", parsed, error));
    CHECK(error.message.find("at least one value") != std::string::npos);
    CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);

    // The shared semantic layer already assigns the collision code, and the
    // throw now passes it through instead of dropping it (W-60): the source
    // twin of the descriptor path's duplicate-symbol refusal.
    CHECK(!easypb_proto::parse_proto("duplicate-symbol.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1; } "
        "message M { optional int32 b = 2; }", parsed, error));
    CHECK(!error.message.empty());
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);

    // W-65: the grammar is asked before the value, exactly as protoc does.
    // The missing `;` is the error even though the value would also be
    // refused, and the empty value follows the same order.
    CHECK(!easypb_proto::parse_proto("proto4-no-semi.proto",
        "syntax = \"proto4\"\npackage s;\n", parsed, error));
    CHECK(error.message.find("expected ';'") != std::string::npos);
    CHECK(error.message.find("syntax must be") == std::string::npos);
    CHECK(error.location.line == 2 && error.location.column == 1);

    CHECK(!easypb_proto::parse_proto("empty-no-semi.proto",
        "syntax = \"\"\nmessage M { optional int32 a = 1; }\n",
        parsed, error));
    CHECK(error.message.find("expected ';'") != std::string::npos);
    CHECK(error.message.find("syntax must be") == std::string::npos);

    // The with-`;` controls keep their own refusals.
    CHECK(!easypb_proto::parse_proto("proto4-with-semi.proto",
        "syntax = \"proto4\";\npackage s;\n", parsed, error));
    CHECK(error.message.find("syntax must be") != std::string::npos);
}

void test_rejects_duplicate_oneof_names()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("oneof.proto",
        "syntax=\"proto3\"; message M { oneof x { int32 a=1; } oneof x { int32 b=2; } }",
        parsed, error));
    CHECK(error.message.find("oneof") != std::string::npos);
}

// D-28 (fix round r13, contract clause 12): a oneof declaration no field points
// at is refused by protoc as source text ("Oneof must have at least one
// field."). The grammar accepts the body, so the refusal comes from the shared
// reference-free pass the parser runs after parsing
// (`easypb_schema::validate_deferred_file` -> `resolve_message`), which is also
// where the descriptor-origin half of the rule lives - one rule, one place, and
// the linker suite's `test_descriptor_origin_empty_oneof_is_rejected` pins the
// other half. The neighbour with a member stays legal, so this is the empty
// declaration and not the oneof keyword.
void test_rejects_an_empty_oneof_body()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("oneof-empty.proto",
        "syntax=\"proto2\"; message M { oneof pick { } }", parsed, error));
    CHECK(error.message.find("oneof") != std::string::npos);
    CHECK(error.message.find("has no fields") != std::string::npos);

    // An option-only body is still an empty one, and the control with a member
    // parses.
    easypb_proto::ParsedProto second;
    easypb_proto::Diagnostic second_error;
    CHECK(!easypb_proto::parse_proto("oneof-option.proto",
        "syntax=\"proto2\"; message M { oneof pick { option (x) = 1; } }",
        second, second_error));

    easypb_proto::ParsedProto control;
    easypb_proto::Diagnostic control_error;
    CHECK(easypb_proto::parse_proto("oneof-ok.proto",
        "syntax=\"proto2\"; message M { oneof pick { int32 a = 1; } }",
        control, control_error));
}

// D-28 (fix round r13): the parser RECORDS `option allow_alias` in the
// descriptor it emits, because the linker validates descriptor-origin enums for
// repeated numbers and cannot tell an intentional alias from that defect
// without it. The present-and-true state is asserted here (an alias the linker
// must accept - the end-to-end fixture is
// `tests/codegen/enums/data/enum_codegen.proto`, whose `STARTED = 1` and
// `ACTIVE = 1` sit under the option). The absent state keeps the parser's own
// duplicate-number refusal as the only guard. Explicit `false` is not a licence
// and is refused as source text (CR-26, `protoc`'s `ValidateEnum`), so the
// parser no longer produces that state; the descriptor path still can, and the
// validity lane carries it.
void test_records_allow_alias_in_the_model()
{
    easypb_proto::ParsedProto aliased;
    easypb_proto::Diagnostic aliased_error;
    CHECK(easypb_proto::parse_proto("alias.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = true; A = 1; B = 1; }",
        aliased, aliased_error));
    CHECK(aliased.file.enum_type.size() == 1);
    if (aliased.file.enum_type.size() == 1) {
        CHECK(aliased.file.enum_type[0].has_options);
        CHECK(aliased.file.enum_type[0].options.has_allow_alias);
        CHECK(aliased.file.enum_type[0].options.allow_alias);
    }

    easypb_proto::ParsedProto plain;
    easypb_proto::Diagnostic plain_error;
    CHECK(easypb_proto::parse_proto("plain.proto",
        "syntax=\"proto2\"; enum E { A = 1; B = 2; }", plain, plain_error));
    CHECK(plain.file.enum_type.size() == 1);
    if (plain.file.enum_type.size() == 1) {
        CHECK(!plain.file.enum_type[0].has_options);
        CHECK(!plain.file.enum_type[0].options.has_allow_alias);
    }

    easypb_proto::ParsedProto explicit_false;
    easypb_proto::Diagnostic false_error;
    CHECK(!easypb_proto::parse_proto("false.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = false; A = 1; B = 2; }",
        explicit_false, false_error));
    CHECK(false_error.message.find("no effect") != std::string::npos);
}


void test_buffer_api_owns_result_strings()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    {
        const std::string source =
            "syntax=\"proto3\"; package owned; message M { string value = 1; }";
        const std::size_t value_offset = source.find("value");
        CHECK(easypb_proto::parse_proto("owned.proto", source.data(), source.size(), parsed, error));
        CHECK(parsed.file.message_type.size() == 1);
        if (!parsed.file.message_type.empty() &&
            !parsed.file.message_type[0].field.empty()) {
            CHECK(parsed.file.message_type[0].field[0].name.data() !=
                  source.data() + value_offset);
        }
    }

    CHECK(parsed.file.has_name && text(parsed.file.name) == "owned.proto");
    CHECK(parsed.file.has_package && text(parsed.file.package) == "owned");
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.empty()) return;
    CHECK(parsed.file.message_type[0].has_name && text(parsed.file.message_type[0].name) == "M");
    CHECK(parsed.file.message_type[0].field.size() == 1);
    if (parsed.file.message_type[0].field.empty()) return;
    CHECK(text(parsed.file.message_type[0].field[0].name) == "value");
}


// D2-A: a custom option is written with a parenthesized first name part. The
// extension that defines it lives in an import this frontend does not resolve,
// so the value stays opaque and is only balanced - but the name grammar is still
// checked, and a leading unparenthesized part is an unknown standard option
// (`Option "file_scope" unknown`, measured with protoc 30.1 on 2026-10-25).
// Every owner kind is exercised, so a name part is consumed at file, message,
// field, oneof, enum, enum-value, service, method and extension-range scope.
void test_custom_option_name_parts_are_consumed()
{
    const char* proto2_source =
        "syntax = \"proto2\";\n"
        "option (vendor.file).(.absolute.path) = true;\n"
        "option (legacy.option).leaf = true;\n"
        "enum E {\n"
        "  option (vendor.enum) = true;\n"
        "  option allow_alias = true;\n"
        "  ZERO = 0 [(vendor.value).leaf = 1];\n"
        "  ALIAS = 0;\n"
        "}\n"
        "message M {\n"
        "  option (vendor.message).leaf = 1;\n"
        "  extensions 100 to 199 [(vendor.range).leaf = true];\n"
        "  repeated int32 values = 1 "
        "      [(vendor.field).(.nested.extension) = 7, packed = true];\n"
        "  oneof choice {\n"
        "    option (vendor.oneof) = true;\n"
        "    string text = 2 [(vendor.oneof_field).leaf = true];\n"
        "  }\n"
        "}\n"
        "service S {\n"
        "  option (vendor.service) = true;\n"
        "  rpc R(M) returns (M) {\n"
        "    option (vendor.method) = true;\n"
        "  }\n"
        "}\n";

    easypb_proto::ParsedProto proto2;
    easypb_proto::Diagnostic proto2_error;
    const bool proto2_ok = easypb_proto::parse_proto(
        "custom-options-proto2.proto", proto2_source, proto2, proto2_error);
    CHECK(proto2_ok);
    if (!proto2_ok) return;

    CHECK(proto2.file.enum_type.size() == 1);
    CHECK(proto2.file.enum_type[0].value.size() == 2);
    CHECK(proto2.file.message_type.size() == 1);
    if (proto2.file.message_type.empty()) return;
    const FieldDescriptorProto* proto2_values =
        find_field(proto2.file.message_type[0], "values");
    CHECK(proto2_values != 0);
    CHECK(proto2_values && proto2_values->has_options);
    CHECK(proto2_values && proto2_values->options.has_packed);
    CHECK(proto2_values && proto2_values->options.packed);

    const char* proto3_source =
        "syntax = \"proto3\";\n"
        "option (vendor.file).tail = true;\n"
        "message P {\n"
        "  repeated int32 values = 1 "
        "      [(vendor.field).(.vendor.inner) = 1, packed = false];\n"
        "}\n";

    easypb_proto::ParsedProto proto3;
    easypb_proto::Diagnostic proto3_error;
    const bool proto3_ok = easypb_proto::parse_proto(
        "custom-options-proto3.proto", proto3_source, proto3, proto3_error);
    CHECK(proto3_ok);
    if (!proto3_ok) return;

    CHECK(proto3.file.message_type.size() == 1);
    if (proto3.file.message_type.empty()) return;
    const FieldDescriptorProto* proto3_values =
        find_field(proto3.file.message_type[0], "values");
    CHECK(proto3_values != 0);
    CHECK(proto3_values && proto3_values->has_options);
    CHECK(proto3_values && proto3_values->options.has_packed);
    CHECK(proto3_values && !proto3_values->options.packed);

    easypb_proto::ParsedProto leading;
    easypb_proto::Diagnostic leading_error;
    CHECK(!easypb_proto::parse_proto(
        "custom-leading-identifier.proto",
        "syntax = \"proto2\";\n"
        "option file_scope.(vendor.file) = true;\n",
        leading, leading_error));
    CHECK(leading_error.message.find("unknown") != std::string::npos);
    CHECK(leading_error.message.find("file_scope") != std::string::npos);
}

void test_easypb_cpp_field_options_are_retained()
{
    const char* source =
        "syntax = \"proto3\";\n"
        "message M {\n"
        "  int32 scalar = 1 [(easypb.cpp).type = \"Wrapped<{}>\"];\n"
        "  repeated string values = 2 [(easypb.cpp).type = \"Small<{},4>\"];\n"
        "  map<string, int32> lookup = 3 [(easypb.cpp).type = \"Flat<{},{}>\"];\n"
        "  int32 ignored = 4 [(other.vendor).type = \"Other\"];\n"
        "}\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const bool ok = easypb_proto::parse_proto("cpp-options.proto", source, parsed, error);
    CHECK(ok);
    if (!ok || parsed.file.message_type.empty()) return;

    const DescriptorProto& message = parsed.file.message_type[0];
    const FieldDescriptorProto* scalar = find_field(message, "scalar");
    const FieldDescriptorProto* values = find_field(message, "values");
    const FieldDescriptorProto* lookup = find_field(message, "lookup");
    const FieldDescriptorProto* ignored = find_field(message, "ignored");

    CHECK(scalar && scalar->has_options && scalar->options.has_cpp);
    CHECK(scalar && scalar->options.cpp.has_type &&
          text(scalar->options.cpp.type) == "Wrapped<{}>");
    CHECK(values && values->has_options && values->options.has_cpp);
    CHECK(values && text(values->options.cpp.type) == "Small<{},4>");
    CHECK(lookup && lookup->has_options && lookup->options.has_cpp);
    CHECK(lookup && text(lookup->options.cpp.type) == "Flat<{},{}>");
    CHECK(ignored && !ignored->has_options && !ignored->options.has_cpp);

    const char* invalid[] = {
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=\"\"]; }",
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=1]; }",
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=\"A\", (easypb.cpp).type=\"B\"]; }",
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).typo=\"A\"]; }",
        "syntax=\"proto3\"; enum E { ZERO=0 [(easypb.cpp).type=\"A\"]; }"
    };
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        easypb_proto::ParsedProto bad;
        easypb_proto::Diagnostic bad_error;
        CHECK(!easypb_proto::parse_proto("bad-cpp-options.proto", invalid[i], bad, bad_error));
        CHECK(!bad_error.message.empty());
    }
}

// R09 (U-54): `(easypb.cpp).type` is a field option. The three field
// spellings normalize to one option name and stay accepted; every other
// declaration scope refuses it with a diagnostic that names the option and the
// scope. Enum value and extension range are the two owners `field_options`
// shares with a field, so they prove the shared bracket list cannot retain a
// value its caller would discard.
void test_cpp_type_option_is_field_only()
{
    const char* accepted[] = {
        "message M { optional int32 a = 1 [(easypb.cpp).type = \"PlainBox\"]; }",
        "message M { optional int32 a = 1 [(.easypb.cpp).type = \"DotBox\"]; }",
        "message M { optional int32 a = 1 [( easypb.cpp ).type = \"SpacedBox\"]; }",
    };
    const char* expected_types[] = { "PlainBox", "DotBox", "SpacedBox" };
    for (std::size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "cpp-type-field.proto",
            std::string("syntax = \"proto2\";\n") + accepted[i], parsed, error);
        if (!ok) std::cerr << accepted[i] << ": " << error.message << '\n';
        CHECK(ok);
        if (!ok || parsed.file.message_type.empty() ||
            parsed.file.message_type[0].field.empty()) {
            continue;
        }
        const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
        CHECK(field.has_options && field.options.has_cpp);
        CHECK(field.options.cpp.has_type);
        CHECK(text(field.options.cpp.type) == expected_types[i]);
    }

    struct ScopeCase
    {
        const char* source;
        const char* scope;
    };
    const ScopeCase refused[] = {
        { "option (easypb.cpp).type = \"A\"; "
          "message M { optional int32 x = 1; }", "file scope" },
        { "message M { option (easypb.cpp).type = \"A\"; "
          "optional int32 x = 1; }", "message scope" },
        { "message M { oneof c { option (easypb.cpp).type = \"A\"; "
          "int32 x = 1; } }", "oneof scope" },
        { "enum E { option (easypb.cpp).type = \"A\"; Z = 0; }", "enum scope" },
        { "enum E { Z = 0 [(easypb.cpp).type = \"A\"]; }", "enum value scope" },
        { "message M { optional int32 x = 1; } "
          "service S { option (easypb.cpp).type = \"A\"; }", "service scope" },
        { "message M { optional int32 x = 1; } "
          "service S { rpc R (M) returns (M) { "
          "option (easypb.cpp).type = \"A\"; } }", "method scope" },
        { "message M { extensions 100 to 199 "
          "[(easypb.cpp).type = \"A\"]; }", "extension range scope" },
    };
    for (std::size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "cpp-type-scope.proto",
            std::string("syntax = \"proto2\";\n") + refused[i].source,
            parsed, error);
        if (ok) {
            std::cerr << "accepted at " << refused[i].scope << ": "
                      << refused[i].source << '\n';
        }
        CHECK(!ok);
        CHECK(error.message.find("(easypb.cpp).type") != std::string::npos);
        CHECK(error.message.find(refused[i].scope) != std::string::npos);
    }
}

void test_malformed_custom_option_name_parts_are_rejected()
{
    const char* malformed[] = {
        "option foo.() = true;",
        "option foo..bar = true;",
        "option foo.(.bar.) = true;",
        "option .foo = true;",
        "option (foo)(bar) = true;"
    };

    for (std::size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto("bad-custom-option.proto", malformed[i], parsed, error));
        CHECK(!error.message.empty());
        CHECK(error.location.line == 1);
    }
}


void test_service_rpc_syntax_is_consumed()
{
    const char* source =
        "syntax = \"proto3\";\n"
        "package rpc.test;\n"
        "message Request { string text = 1; }\n"
        "message Reply { string text = 1; }\n"
        "service Greeter {\n"
        "  option deprecated = true;\n"
        "  rpc Unary(Request) returns (Reply);\n"
        "  rpc ClientStream(stream .rpc.test.Request) returns (Reply) {\n"
        "    option deprecated = true;\n"
        "  }\n"
        "  rpc ServerStream(Request) returns (stream Reply);\n"
        "  rpc Bidi(stream Request) returns (stream Reply) {}\n"
        "}\n"
        "message AfterService { int32 value = 1; }\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("rpc.proto", source, parsed, error));
    CHECK(parsed.file.message_type.size() == 3);
    if (parsed.file.message_type.size() != 3) return;
    CHECK(text(parsed.file.message_type[0].name) == "Request");
    CHECK(text(parsed.file.message_type[1].name) == "Reply");
    CHECK(text(parsed.file.message_type[2].name) == "AfterService");
}

// U-38: protoc records a streaming field only when the `stream` keyword is
// spelled (`ParseServiceMethod`, parser.cc:2281-2286, 2301-2307); a unary
// method leaves both fields absent. The has_* flags are the presence bits a
// descriptor writer would carry, so a non-streaming method must not look like
// an explicit `stream` in the trimmed model.
void test_rpc_streaming_presence_flags()
{
    const char* source =
        "syntax = \"proto3\";\n"
        "message Request { string text = 1; }\n"
        "message Reply { string text = 1; }\n"
        "service S {\n"
        "  rpc Unary(Request) returns (Reply);\n"
        "  rpc Client(stream Request) returns (Reply);\n"
        "  rpc Server(Request) returns (stream Reply);\n"
        "  rpc Bidi(stream Request) returns (stream Reply);\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("rpc-presence.proto", source, parsed, error));
    CHECK(parsed.file.service.size() == 1);
    if (parsed.file.service.size() != 1) return;
    const ServiceDescriptorProto& service = parsed.file.service[0];
    CHECK(service.method.size() == 4);
    if (service.method.size() != 4) return;
    struct Row
    {
        bool client;
        bool server;
    };
    static const Row rows[] = {
        {false, false},
        {true, false},
        {false, true},
        {true, true}
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        CHECK(service.method[i].client_streaming == rows[i].client);
        CHECK(service.method[i].server_streaming == rows[i].server);
        CHECK(service.method[i].has_client_streaming == rows[i].client);
        CHECK(service.method[i].has_server_streaming == rows[i].server);
    }
}

void test_service_is_rejected_inside_message()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto(
        "bad-rpc.proto",
        "syntax=\"proto3\"; message M { service S {} }",
        parsed,
        error));
    CHECK(error.message.find("service") != std::string::npos);
}

void test_rejects_service_scope_name_collisions()
{
    const char* collisions[] = {
        "syntax=\"proto3\"; message S {} service S {}",
        "syntax=\"proto3\"; enum S { X = 0; } service S {}",
        "syntax=\"proto3\"; enum E { S = 0; } service S {}",
        "syntax=\"proto3\"; service S {} service S {}"
    };

    for (std::size_t i = 0; i < sizeof(collisions) / sizeof(collisions[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto(
            "service-name-collision.proto", collisions[i], parsed, error));
        CHECK(error.message.find("service") != std::string::npos);
    }
}

void test_rejects_malformed_rpc_syntax()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto(
        "bad-rpc.proto",
        "syntax=\"proto3\"; message A {} message B {} service S { rpc R(A) B; }",
        parsed,
        error));
    CHECK(error.message.find("returns") != std::string::npos);
}

void test_decode_all_field_descriptor_members()
{
    const unsigned char encoded[] = {
        0x0a, 0x01, 'x',             // name = x
        0x18, 0x07,                  // number = 7
        0x20, 0x03,                  // label = repeated
        0x28, 0x0e,                  // type = enum
        0x32, 0x02, '.', 'E',        // type_name = .E
        0x3a, 0x01, 'A',             // default_value = A
        0x42, 0x02, 0x10, 0x01,      // options { packed = true }
        0x48, 0x02                   // oneof_index = 2
    };
    const std::string buffer(reinterpret_cast<const char*>(encoded), sizeof(encoded));
    FieldDescriptorProto field = easypb::decode<FieldDescriptorProto>(buffer);
    CHECK(field.has_name && text(field.name) == "x");
    CHECK(field.has_number && field.number == 7);
    CHECK(field.has_label && field.label == FieldDescriptorProto::LABEL_REPEATED);
    CHECK(field.has_type && field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(field.has_type_name && text(field.type_name) == ".E");
    CHECK(field.has_default_value && text(field.default_value) == "A");
    CHECK(field.has_options && field.options.has_packed && field.options.packed);
    CHECK(field.has_oneof_index && field.oneof_index == 2);
}


void test_decode_complete_descriptor_set()
{
    const unsigned char encoded[] = {
        0x0a, 0x59, 0x0a, 0x09, 0x61, 0x6c, 0x6c, 0x2e, 0x70, 0x72, 0x6f, 0x74,
        0x6f, 0x12, 0x01, 0x70, 0x22, 0x3a, 0x0a, 0x01, 0x4d, 0x12, 0x1b, 0x0a,
        0x01, 0x78, 0x18, 0x01, 0x20, 0x01, 0x28, 0x0e, 0x32, 0x04, 0x2e, 0x70,
        0x2e, 0x45, 0x3a, 0x04, 0x5a, 0x45, 0x52, 0x4f, 0x42, 0x02, 0x10, 0x00,
        0x48, 0x00, 0x1a, 0x0e, 0x0a, 0x08, 0x4d, 0x61, 0x70, 0x45, 0x6e, 0x74,
        0x72, 0x79, 0x3a, 0x02, 0x38, 0x01, 0x42, 0x08, 0x0a, 0x06, 0x63, 0x68,
        0x6f, 0x69, 0x63, 0x65, 0x2a, 0x0d, 0x0a, 0x01, 0x45, 0x12, 0x08, 0x0a,
        0x04, 0x5a, 0x45, 0x52, 0x4f, 0x10, 0x00
    };
    const std::string buffer(reinterpret_cast<const char*>(encoded), sizeof(encoded));
    const FileDescriptorSet set = easypb::decode<FileDescriptorSet>(buffer);
    CHECK(set.file.size() == 1);
    if (set.file.empty()) return;

    const FileDescriptorProto& file = set.file[0];
    CHECK(file.has_name && text(file.name) == "all.proto");
    CHECK(file.has_package && text(file.package) == "p");
    CHECK(file.enum_type.size() == 1);
    CHECK(file.enum_type[0].has_name && text(file.enum_type[0].name) == "E");
    CHECK(file.enum_type[0].value.size() == 1);
    CHECK(file.enum_type[0].value[0].has_number && file.enum_type[0].value[0].number == 0);
    CHECK(file.message_type.size() == 1);
    if (file.message_type.empty()) return;

    const DescriptorProto& message = file.message_type[0];
    CHECK(message.has_name && text(message.name) == "M");
    CHECK(message.oneof_decl.size() == 1);
    CHECK(message.oneof_decl[0].has_name && text(message.oneof_decl[0].name) == "choice");
    CHECK(message.nested_type.size() == 1);
    CHECK(message.nested_type[0].has_options);
    CHECK(message.nested_type[0].options.has_map_entry && message.nested_type[0].options.map_entry);
    CHECK(message.field.size() == 1);
    if (message.field.empty()) return;

    const FieldDescriptorProto& field = message.field[0];
    CHECK(field.has_type_name && text(field.type_name) == ".p.E");
    CHECK(field.has_default_value && text(field.default_value) == "ZERO");
    CHECK(field.has_options && field.options.has_packed && !field.options.packed);
    CHECK(field.has_oneof_index && field.oneof_index == 0);
}


void test_decode_file_syntax()
{
    const char encoded[] = {
        '\x0a', '\x07', 'x', '.', 'p', 'r', 'o', 't', 'o',
        '\x62', '\x06', 'p', 'r', 'o', 't', 'o', '3'
    };
    FileDescriptorProto file;
    decode(easypb::Decoder(encoded, sizeof(encoded)), file);
    CHECK(file.has_name && text(file.name) == "x.proto");
    CHECK(file.has_syntax && text(file.syntax) == "proto3");
}

void test_imports_populate_descriptor_dependencies()
{
    const std::string source =
        "syntax = \"proto2\";\n"
        "import \"a.proto\";\n"
        "import public \"b.proto\";\n"
        "import weak \"c.proto\";\n"
        "message Root {}\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    // The standalone parser records imports without opening the files.
    CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error));
    CHECK(parsed.file.dependency.size() == 3);
    if (parsed.file.dependency.size() == 3) {
        CHECK(text(parsed.file.dependency[0]) == "a.proto");
        CHECK(text(parsed.file.dependency[1]) == "b.proto");
        CHECK(text(parsed.file.dependency[2]) == "c.proto");
    }
    CHECK(parsed.file.public_dependency.size() == 1);
    if (!parsed.file.public_dependency.empty()) {
        CHECK(parsed.file.public_dependency[0] == 1);
    }
    CHECK(parsed.file.weak_dependency.size() == 1);
    if (!parsed.file.weak_dependency.empty()) {
        CHECK(parsed.file.weak_dependency[0] == 2);
    }
    CHECK(parsed.imports.size() == 3);
    if (parsed.imports.size() == 3) {
        CHECK(parsed.imports[0].path == "a.proto");
        CHECK(parsed.imports[0].modifier == easypb_proto::ImportInfo::NORMAL_IMPORT);
        CHECK(parsed.imports[1].path == "b.proto");
        CHECK(parsed.imports[1].modifier == easypb_proto::ImportInfo::PUBLIC_IMPORT);
        CHECK(parsed.imports[2].path == "c.proto");
        CHECK(parsed.imports[2].modifier == easypb_proto::ImportInfo::WEAK_IMPORT);
    }
}

void test_package_declaration_location_is_recorded()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("packaged.proto",
        "syntax = \"proto2\";\n\npackage p;\nmessage Value {}\n",
        parsed, error));
    easypb_schema::DescriptorPath package_path;
    package_path.push_back(2);
    const std::map<easypb_schema::DescriptorPath,
                   easypb_proto::SourceLocation>::const_iterator found =
        parsed.declaration_locations.find(package_path);
    CHECK(found != parsed.declaration_locations.end());
    if (found != parsed.declaration_locations.end()) {
        CHECK(found->second.line == 3 && found->second.column == 9);
    }
}

void test_service_names_are_retained()
{
    const std::string source =
        "syntax = \"proto3\";\n"
        "message Request { string text = 1; }\n"
        "service Greeter {\n"
        "  rpc Unary(Request) returns (Request);\n"
        "}\n";

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("greeter.proto", source, parsed, error));
    CHECK(parsed.file.service.size() == 1);
    if (!parsed.file.service.empty()) {
        CHECK(parsed.file.service[0].has_name);
        CHECK(text(parsed.file.service[0].name) == "Greeter");
    }
}

void test_deferred_leaves_imported_types_unresolved()
{
    const std::string source =
        "syntax=\"proto2\"; import \"state.proto\"; "
        "message M { repeated State states=1 [packed=true]; }";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    easypb_proto::ParseOptions options;
    options.defer_type_resolution = true;
    CHECK(easypb_proto::parse_proto("root.proto", source.data(), source.size(),
                                   parsed, error, options));
    if (parsed.file.message_type.empty()) return;
    const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
    CHECK(field.has_type_name);
    CHECK(!field.has_type);
    CHECK(std::string(field.type_name.data(), field.type_name.size()) == "State");
    CHECK(field.options.has_packed && field.options.packed);
    // Deferred output leaves imports silently unresolved for the loader.
    for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
        CHECK(parsed.warnings[i].code != easypb_proto::DIAGNOSTIC_UNRESOLVED_TYPE);
    }
    CHECK(parsed.file.dependency.size() == 1);
}

void test_standalone_keeps_unresolved_placeholder_and_warnings()
{
    const std::string source =
        "syntax=\"proto2\"; import \"state.proto\"; "
        "message M { repeated State states=1 [packed=true]; }";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error));
    if (parsed.file.message_type.empty()) return;
    const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
    CHECK(field.has_type_name);
    CHECK(field.has_type && field.type == FieldDescriptorProto::TYPE_MESSAGE);
    bool saw_unresolved = false;
    for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
        if (parsed.warnings[i].code == easypb_proto::DIAGNOSTIC_UNRESOLVED_TYPE) {
            saw_unresolved = true;
        }
    }
    CHECK(saw_unresolved);
}

void test_deferred_defers_kind_dependent_checks()
{
    easypb_proto::Diagnostic error;
    {
        // A locally invalid enum default still fails in standalone mode...
        easypb_proto::ParsedProto parsed;
        CHECK(!easypb_proto::parse_proto("bad-enum-default.proto",
            "enum E { A = 0; } message M { optional E e = 1 [default = B]; }",
            parsed, error));
        CHECK(error.message.find("no value named B") != std::string::npos);
    }
    {
        // ...but passes in deferred mode: shadowing imports may change the
        // resolved kind after linking, so even a locally visible kind must
        // not decide default validity ahead of linking.
        easypb_proto::ParsedProto parsed;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(easypb_proto::parse_proto("bad-enum-default.proto",
            "enum E { A = 0; } message M { optional E e = 1 [default = B]; }",
            parsed, error, options));
        if (parsed.file.message_type.empty()) return;
        const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
        CHECK(field.has_type_name && !field.has_type);
        CHECK(text(field.type_name) == "E");
        CHECK(field.has_default_value && text(field.default_value) == "B");
        CHECK(parsed.warnings.empty());
    }
    {
        // Syntax-only failures stay immediate in deferred mode.
        easypb_proto::ParsedProto parsed;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(!easypb_proto::parse_proto("duplicate-field.proto",
            "message M { optional int32 x = 1; optional int32 y = 1; }",
            parsed, error, options));
        CHECK(error.message.find("duplicate field number") != std::string::npos);
    }
    {
        // A locally resolvable reference keeps its raw spelling in deferred
        // output instead of the absolute name.
        easypb_proto::ParsedProto parsed;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(easypb_proto::parse_proto("local.proto",
            "package p; message N {} message M { optional N child = 1; }",
            parsed, error, options));
        if (parsed.file.message_type.size() == 2) {
            const FieldDescriptorProto* child =
                find_field(parsed.file.message_type[1], "child");
            CHECK(child != 0);
            CHECK(child && child->has_type_name && !child->has_type);
            CHECK(child && text(child->type_name) == "N");
        } else {
            CHECK(parsed.file.message_type.size() == 2);
        }
    }
}

void test_deferred_ignores_shadowed_local_kind()
{
    // A local message E must not decide packed/default validity in deferred
    // mode: after linking, "b.E" resolves to the imported enum .a.b.b.E,
    // which accepts both. protoc 30.1 accepts this schema.
    const std::string source =
        "syntax = \"proto2\"; package a.b; import \"shadow.proto\"; "
        "message E {} "
        "message M {"
        " repeated b.E items = 1 [packed = true];"
        " optional b.E choice = 2 [default = B];"
        "}";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    easypb_proto::ParseOptions options;
    options.defer_type_resolution = true;
    CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error, options));
    if (parsed.file.message_type.size() != 2) {
        CHECK(parsed.file.message_type.size() == 2);
        return;
    }
    const FieldDescriptorProto* items = find_field(parsed.file.message_type[1], "items");
    CHECK(items != 0);
    CHECK(items && items->has_type_name && !items->has_type);
    CHECK(items && text(items->type_name) == "b.E");
    CHECK(items && items->options.has_packed && items->options.packed);
    const FieldDescriptorProto* choice = find_field(parsed.file.message_type[1], "choice");
    CHECK(choice != 0);
    CHECK(choice && choice->has_type_name && !choice->has_type);
    CHECK(choice && choice->has_default_value && text(choice->default_value) == "B");
    CHECK(parsed.warnings.empty());
}

void test_deferred_nested_named_fields_stay_silent()
{
    const std::string source =
        "syntax=\"proto2\"; import \"other.proto\"; "
        "message Outer { message Inner { optional Foo f = 1 [default = BAR]; } }";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    easypb_proto::ParseOptions options;
    options.defer_type_resolution = true;
    CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error, options));
    CHECK(parsed.file.message_type.size() == 1);
    if (parsed.file.message_type.size() != 1) return;
    CHECK(parsed.file.message_type[0].nested_type.size() == 1);
    if (parsed.file.message_type[0].nested_type.empty()) return;
    const DescriptorProto& inner = parsed.file.message_type[0].nested_type[0];
    CHECK(inner.field.size() == 1);
    if (inner.field.empty()) return;
    const FieldDescriptorProto& field = inner.field[0];
    CHECK(field.has_type_name && !field.has_type);
    CHECK(text(field.type_name) == "Foo");
    CHECK(field.has_default_value && text(field.default_value) == "BAR");
    CHECK(parsed.warnings.empty());
}

void test_packed_requires_repeated_label()
{
    // Repeatedness needs no type resolution: no import can turn an optional
    // field into a repeated one, so packed on a non-repeated field fails in
    // both modes even when the type is unknown.
    const std::string source =
        "syntax=\"proto2\"; import \"state.proto\"; "
        "message M { optional State s = 1 [packed = true]; }";
    easypb_proto::Diagnostic error;
    {
        easypb_proto::ParsedProto parsed;
        CHECK(!easypb_proto::parse_proto("root.proto", source, parsed, error));
        CHECK(error.message.find("packed option is valid only") != std::string::npos);
    }
    {
        easypb_proto::ParsedProto parsed;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(!easypb_proto::parse_proto("root.proto", source, parsed, error, options));
        CHECK(error.message.find("packed option is valid only") != std::string::npos);
    }
}

void test_unresolved_default_is_deferred_not_rejected()
{
    const std::string source =
        "syntax=\"proto2\"; import \"other.proto\"; "
        "message M { optional Other o = 1 [default = FOO]; }";
    {
        // Deferred: an unresolvable named type keeps its raw spelling with
        // has_type == false, and its default is left for complete linking
        // instead of failing on the unknown kind.
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error, options));
        if (parsed.file.message_type.empty()) return;
        const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
        CHECK(field.has_type_name && !field.has_type);
        CHECK(text(field.type_name) == "Other");
        CHECK(field.has_default_value && text(field.default_value) == "FOO");
        for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
            CHECK(parsed.warnings[i].code != easypb_proto::DIAGNOSTIC_UNRESOLVED_TYPE);
        }
    }
    {
        // Standalone: the same schema parses with the compatibility
        // placeholder and an unresolved diagnostic instead of an error.
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error));
        if (parsed.file.message_type.empty()) return;
        const FieldDescriptorProto& field = parsed.file.message_type[0].field[0];
        CHECK(field.has_type && field.type == FieldDescriptorProto::TYPE_MESSAGE);
        bool saw_default_warning = false;
        for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
            if (parsed.warnings[i].message.find("cannot validate default") !=
                std::string::npos) {
                saw_default_warning = true;
            }
        }
        CHECK(saw_default_warning);
    }
}

void test_unresolved_warnings_are_per_field()
{
    const std::string source =
        "syntax=\"proto2\"; import \"state.proto\"; "
        "message M { optional State a = 1; optional State b = 2; }";
    {
        // Standalone: each field warns once, at its own position.
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error));
        std::size_t unresolved = 0;
        for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
            if (parsed.warnings[i].code == easypb_proto::DIAGNOSTIC_UNRESOLVED_TYPE) {
                ++unresolved;
            }
        }
        CHECK(unresolved == 2);
    }
    {
        // Deferred: imports stay silently unresolved.
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        easypb_proto::ParseOptions options;
        options.defer_type_resolution = true;
        CHECK(easypb_proto::parse_proto("root.proto", source, parsed, error, options));
        CHECK(parsed.warnings.empty());
    }
}

void test_parsed_metadata_survives_source_destruction()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    {
        const std::string source =
            "syntax = \"proto2\";\n"
            "package demo;\n"
            "import \"a.proto\";\n"
            "enum State { READY = 7; }\n"
            "message M {\n"
            "  optional State state = 1 [default = READY];\n"
            "  map<string, int32> counts = 2;\n"
            "}\n";
        CHECK(easypb_proto::parse_proto("dying.proto", source, parsed, error));
    }
    // The source buffer is destroyed here; every retained string, raw type
    // spelling, and descriptor must remain readable.
    CHECK(parsed.file.has_package && text(parsed.file.package) == "demo");
    CHECK(parsed.file.dependency.size() == 1);
    CHECK(text(parsed.file.dependency[0]) == "a.proto");
    CHECK(!parsed.field_sources.empty());
    CHECK(!parsed.declaration_locations.empty());
    if (parsed.file.message_type.empty()) return;
    const DescriptorProto& message = parsed.file.message_type[0];
    const FieldDescriptorProto* state = find_field(message, "state");
    CHECK(state != 0);
    CHECK(state && state->type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(state && text(state->type_name) == ".demo.State");
    CHECK(state && state->has_default_value && text(state->default_value) == "READY");
    const FieldDescriptorProto* counts = find_field(message, "counts");
    CHECK(counts != 0);
    CHECK(counts && counts->type == FieldDescriptorProto::TYPE_MESSAGE);
}

// W-15: an explicit empty syntax value is not the absent descriptor syntax.
void test_empty_syntax_statement_is_refused()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("empty-syntax.proto",
        "syntax = \"\"; message M { optional int32 x = 1; }", parsed, error));
    CHECK(error.message.find("syntax must be") != std::string::npos);
    CHECK(error.location.line == 1);
    CHECK(error.location.column == 10);
    // S7e (W-60): the empty value is a source-only rule - the descriptor path
    // reads an empty/absent syntax as proto2 - so there is no twin to match
    // and the refusal keeps DIAGNOSTIC_GENERIC.
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);

    CHECK(easypb_proto::parse_proto("proto2.proto",
        "syntax = \"proto2\"; message M { optional int32 x = 1; }", parsed, error));
    CHECK(easypb_proto::parse_proto("proto3.proto",
        "syntax = \"proto3\"; message M { int32 x = 1; }", parsed, error));
    CHECK(easypb_proto::parse_proto("absent.proto",
        "message M { optional int32 x = 1; }", parsed, error));
}

// W-41: the lexer consumes a leading BOM and leaves the same syntax and
// declarations as the no-BOM twin.
void test_bom_is_consumed_by_the_lexer()
{
    const std::string source =
        "syntax = \"proto3\";\n"
        "message M { int32 x = 1; }\n";
    easypb_proto::ParsedProto plain;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("plain.proto", source, plain, error));

    easypb_proto::ParsedProto bom;
    const std::string with_bom = "\xEF\xBB\xBF" + source;
    CHECK(easypb_proto::parse_proto("bom.proto", with_bom, bom, error));
    CHECK(bom.file.has_syntax);
    CHECK(text(bom.file.syntax) == "proto3");
    CHECK(bom.file.message_type.size() == plain.file.message_type.size());
}

// W-04: the parser stores the canonical text protoc writes, including the
// ERANGE family; a normal value is unchanged.
void test_float_defaults_are_canonicalized()
{
    const std::string source =
        "syntax = \"proto2\";\n"
        "message M {\n"
        "  optional double overflow = 1 [default = 1e400];\n"
        "  optional double underflow = 2 [default = 1e-400];\n"
        "  optional double negative_underflow = 3 [default = -1e-400];\n"
        "  optional double subnormal = 4 [default = 1e-320];\n"
        "  optional double normal = 5 [default = 1.5];\n"
        "}\n";
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("floats.proto", source, parsed, error));
    const DescriptorProto& m = parsed.file.message_type[0];
    CHECK(text(find_field(m, "overflow")->default_value) == "inf");
    CHECK(text(find_field(m, "underflow")->default_value) == "0");
    CHECK(text(find_field(m, "negative_underflow")->default_value) == "-0");
    CHECK(text(find_field(m, "subnormal")->default_value) ==
          "9.99988867182683e-321");
    CHECK(text(find_field(m, "normal")->default_value) == "1.5");
}

// W-39: the source tokenizer and the bytes escaping agree on the uppercase
// hex escape. The source width stays at two digits.
void test_bytes_upper_hex_escape_is_decoded()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const std::string source =
        "syntax = \"proto2\";\n"
        "message M {\n"
        "  optional bytes upper = 1 [default = \"\\X41\"];\n"
        "  optional bytes lower = 2 [default = \"\\x41\"];\n"
        "  optional bytes narrow = 3 [default = \"\\x041\"];\n"
        "}\n";
    CHECK(easypb_proto::parse_proto("bytes.proto", source, parsed, error));
    const DescriptorProto& m = parsed.file.message_type[0];
    CHECK(text(find_field(m, "upper")->default_value) == "A");
    CHECK(text(find_field(m, "lower")->default_value) == "A");
    CHECK(text(find_field(m, "narrow")->default_value) == "\\0041");
}

// W-40 (option B): every leading plus on a numeric default or enum value is
// refused; the controls stay accepted.
void test_leading_plus_is_refused()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("plus-int.proto",
        "syntax = \"proto2\"; message M { optional int32 x = 1 [default = +7]; }",
        parsed, error));
    CHECK(error.message.find("Expected integer") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("plus-enum.proto",
        "syntax = \"proto2\"; enum E { A = +0; }", parsed, error));
    CHECK(error.message.find("Expected integer") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("plus-float.proto",
        "syntax = \"proto2\"; message M { optional double x = 1 [default = +1.0]; }",
        parsed, error));
    CHECK(error.message.find("Expected number") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("plus-inf.proto",
        "syntax = \"proto2\"; message M { optional double x = 1 [default = +inf]; }",
        parsed, error));
    CHECK(error.message.find("Expected number") != std::string::npos);

    CHECK(easypb_proto::parse_proto("controls.proto",
        "syntax = \"proto2\"; enum E { A = 0; } "
        "message M { optional int32 x = 1 [default = 7]; "
        "optional double d = 2 [default = 1.0]; "
        "optional double i = 3 [default = inf]; "
        "optional double n = 4 [default = nan]; "
        "optional E e = 5 [default = A]; }",
        parsed, error));
}

// W-43: the source option is retained and its shape is checked by the parser.
// CR-11/20/26/27/28 (B7): the statement and option grammar table. One valid
// and one invalid spelling per rule, all against protoc 30.1. The acceptance
// controls prove the refusal is the rule and not a blanket ban; the refusal
// rows assert the verdict (the words are the parser's own).
void test_statement_and_option_grammar_table()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    // CR-11 valid: a oneof member carries an explicit default, like any other
    // optional field. The descriptor path accepts the same shape.
    CHECK(easypb_proto::parse_proto("oneof-default.proto",
        "syntax=\"proto2\"; message M { oneof choice { int32 a = 1 [default = 7]; } }",
        parsed, error));
    const FieldDescriptorProto* oneof_int =
        find_field(parsed.file.message_type[0], "a");
    CHECK(oneof_int != 0);
    CHECK(oneof_int && oneof_int->has_oneof_index && oneof_int->oneof_index == 0);
    CHECK(oneof_int && oneof_int->has_default_value &&
          text(oneof_int->default_value) == "7");
    CHECK(easypb_proto::parse_proto("oneof-default-string.proto",
        "syntax=\"proto2\"; message M { oneof choice { string s = 1 [default = \"x\"]; } }",
        parsed, error));
    CHECK(find_field(parsed.file.message_type[0], "s") != 0);

    // CR-11 invalid: the repeated refusal stays.
    CHECK(!easypb_proto::parse_proto("repeated-default.proto",
        "syntax=\"proto2\"; message M { repeated int32 a = 1 [default = 7]; }",
        parsed, error));
    CHECK(error.message.find("repeated") != std::string::npos);

    // CR-20 valid: empty statements after `syntax` stay legal, and a leading
    // empty statement is legal when no syntax declaration follows it (protoc
    // defaults the file to proto2).
    CHECK(easypb_proto::parse_proto("empty-after-syntax.proto",
        "syntax=\"proto2\"; ; message M { optional int32 a = 1; }",
        parsed, error));
    CHECK(easypb_proto::parse_proto("leading-semicolon-message.proto",
        "; message M { optional int32 a = 1; }", parsed, error));
    CHECK(easypb_proto::parse_proto("syntax-after-comment.proto",
        "// leading comment\nsyntax=\"proto2\"; message M { optional int32 a = 1; }",
        parsed, error));

    // CR-20 invalid: `syntax` is recognized only as the first token.
    CHECK(!easypb_proto::parse_proto("leading-semicolon-syntax.proto",
        "; syntax=\"proto2\"; message M { optional int32 a = 1; }",
        parsed, error));
    CHECK(error.message.find("first statement") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("two-semicolons-syntax.proto",
        ";; syntax=\"proto2\"; message M { optional int32 a = 1; }",
        parsed, error));
    CHECK(error.message.find("first statement") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("syntax-after-package.proto",
        "syntax=\"proto2\"; package p; syntax=\"proto2\";", parsed, error));
    CHECK(error.message.find("first statement") != std::string::npos);

    // CR-26 valid: `true` licenses a shared number.
    CHECK(easypb_proto::parse_proto("allow-alias-valid.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = true; A = 0; B = 0; }",
        parsed, error));

    // CR-26 invalid: `true` without an alias is unnecessary, and an explicit
    // `false` has no effect (with or without an alias).
    CHECK(!easypb_proto::parse_proto("allow-alias-useless.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = true; A = 0; B = 1; }",
        parsed, error));
    CHECK(error.message.find("no enum values share field numbers") !=
          std::string::npos);
    CHECK(!easypb_proto::parse_proto("allow-alias-false.proto",
        "syntax=\"proto2\"; enum E { A = 0; B = 1; option allow_alias = false; }",
        parsed, error));
    CHECK(error.message.find("no effect") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("allow-alias-false-with-alias.proto",
        "syntax=\"proto2\"; enum E { A = 0; B = 0; option allow_alias = false; }",
        parsed, error));
    CHECK(error.message.find("no effect") != std::string::npos);

    // CR-27 valid: an ordinary enum value has no options.
    CHECK(easypb_proto::parse_proto("enum-control.proto",
        "syntax=\"proto2\"; enum E { A = 0; }", parsed, error));

    // CR-27 invalid: `json_name` is a field option, not an enum-value option.
    // D2-A moved the refusal into the shared table, which reports an unknown
    // name for the owner.
    CHECK(!easypb_proto::parse_proto("enum-json-name.proto",
        "syntax=\"proto2\"; enum E { A = 0 [json_name = \"aName\"]; }",
        parsed, error));
    CHECK(error.message.find("json_name") != std::string::npos);
    CHECK(error.message.find("unknown") != std::string::npos);

    // CR-28 valid: a negative ordinary option value keeps its sign, and a
    // plain field option is unaffected.
    CHECK(easypb_proto::parse_proto("option-minus.proto",
        "syntax=\"proto2\"; message M { optional int32 a = 1; } "
        "option (my.opt) = -7;", parsed, error));
    CHECK(easypb_proto::parse_proto("field-option-control.proto",
        "syntax=\"proto2\"; message M { repeated int32 a = 1 [packed = true]; }",
        parsed, error));

    // CR-28 invalid: every ordinary option refuses a leading `+`. The field
    // `default` is the one exception (E1/W-40), checked by
    // test_leading_plus_is_refused.
    CHECK(!easypb_proto::parse_proto("option-plus-custom.proto",
        "syntax=\"proto2\"; message M { optional int32 a = 1; } "
        "option (my.opt) = +7;", parsed, error));
    CHECK(error.message.find("Expected option value") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("option-plus-known.proto",
        "syntax=\"proto2\"; message M { optional int32 a = 1; } "
        "option java_package = +7;", parsed, error));
    CHECK(error.message.find("Expected option value") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("field-option-plus.proto",
        "syntax=\"proto2\"; message M { repeated int32 a = 1 [packed = +true]; }",
        parsed, error));
    CHECK(error.message.find("Expected option value") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("enum-option-plus.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = +true; A = 0; }",
        parsed, error));
    CHECK(error.message.find("Expected option value") != std::string::npos);
}

void test_json_name_option_is_retained()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const std::string source =
        "syntax = \"proto2\";\n"
        "message M { optional int32 a = 1 [json_name = \"alpha\"]; }\n";
    CHECK(easypb_proto::parse_proto("json.proto", source, parsed, error));
    const FieldDescriptorProto* field =
        find_field(parsed.file.message_type[0], "a");
    CHECK(field != 0 && field->has_json_name);
    CHECK(field && text(field->json_name) == "alpha");

    CHECK(!easypb_proto::parse_proto("json-dup.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1 "
        "[json_name = \"a\", json_name = \"b\"]; }",
        parsed, error));
    CHECK(error.message.find("duplicate json_name") != std::string::npos);

    CHECK(!easypb_proto::parse_proto("json-token.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1 [json_name = 1]; }",
        parsed, error));
    CHECK(error.message.find("json_name must be a string literal") !=
          std::string::npos);

    // A map field expands to a repeated synthetic-entry field; the custom
    // name belongs to the source field that the user wrote.
    CHECK(easypb_proto::parse_proto("map-json.proto",
        "syntax = \"proto3\"; message M { "
        "map<string, int32> m = 1 [json_name = \"alpha\"]; }",
        parsed, error));
    const FieldDescriptorProto* map =
        find_field(parsed.file.message_type[0], "m");
    CHECK(map != 0 && map->has_json_name);
    CHECK(map && text(map->json_name) == "alpha");
}

// D1-A: `protoc` gives a proto3 `optional` field explicit presence by
// synthesizing a one-field oneof (`GenerateSyntheticOneofs`, parser.cc:745-780):
// the candidate starts as the field name, gains one leading `_` unless it has
// one, and grows an `X` per collision with any field or oneof name; synthetic
// declarations come after every real oneof. The parser must produce the same
// model, because `--print-descriptor` and the descriptor path must agree.
void test_proto3_optional_synthetic_oneofs()
{
    // Plain: `a` -> `_a`, field claims index 0.
    easypb_proto::ParsedProto plain;
    easypb_proto::Diagnostic plain_error;
    CHECK(easypb_proto::parse_proto("opt-plain.proto",
        "syntax = \"proto3\";\n"
        "package p;\n"
        "message M { optional int32 a = 1; }\n", plain, plain_error));
    CHECK(plain.file.message_type.size() == 1);
    if (!plain.file.message_type.empty()) {
        const DescriptorProto& message = plain.file.message_type[0];
        CHECK(message.oneof_decl.size() == 1);
        if (message.oneof_decl.size() == 1) {
            CHECK(text(message.oneof_decl[0].name) == "_a");
        }
        CHECK(message.field.size() == 1);
        if (message.field.size() == 1) {
            CHECK(message.field[0].has_proto3_optional);
            CHECK(message.field[0].proto3_optional);
            CHECK(message.field[0].has_oneof_index);
            CHECK(message.field[0].oneof_index == 0);
        }
    }

    // A field already named `_a` pushes the candidate to `X_a`.
    easypb_proto::ParsedProto collision;
    easypb_proto::Diagnostic collision_error;
    CHECK(easypb_proto::parse_proto("opt-field-underscore.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 _a = 1; optional int32 a = 2; }\n",
        collision, collision_error));
    if (!collision.file.message_type.empty()) {
        const DescriptorProto& message = collision.file.message_type[0];
        CHECK(message.oneof_decl.size() == 1);
        if (message.oneof_decl.size() == 1) {
            CHECK(text(message.oneof_decl[0].name) == "X_a");
        }
        const FieldDescriptorProto* a = find_field(message, "a");
        CHECK(a != 0);
        CHECK(a && a->oneof_index == 0 && a->has_oneof_index);
    }

    // The optional field itself may be named `_a`; its candidate is its own
    // name, so it collides with the field and grows one `X` (measured with
    // protoc 30.1: `optional int32 _a = 1;` declares `X_a`).
    easypb_proto::ParsedProto underscored;
    easypb_proto::Diagnostic underscored_error;
    CHECK(easypb_proto::parse_proto("opt-underscored-field.proto",
        "syntax = \"proto3\";\n"
        "message M { optional int32 _a = 1; }\n",
        underscored, underscored_error));
    if (!underscored.file.message_type.empty()) {
        const DescriptorProto& message = underscored.file.message_type[0];
        CHECK(message.oneof_decl.size() == 1);
        if (message.oneof_decl.size() == 1) {
            CHECK(text(message.oneof_decl[0].name) == "X_a");
        }
        const FieldDescriptorProto* field = find_field(message, "_a");
        CHECK(field != 0 && field->oneof_index == 0);
    }

    // A real oneof named `_a` is a collision too, and stays first.
    easypb_proto::ParsedProto real;
    easypb_proto::Diagnostic real_error;
    CHECK(easypb_proto::parse_proto("opt-real-oneof.proto",
        "syntax = \"proto3\";\n"
        "message M { oneof _a { int32 b = 1; } optional int32 a = 2; }\n",
        real, real_error));
    if (!real.file.message_type.empty()) {
        const DescriptorProto& message = real.file.message_type[0];
        CHECK(message.oneof_decl.size() == 2);
        if (message.oneof_decl.size() == 2) {
            CHECK(text(message.oneof_decl[0].name) == "_a");
            CHECK(text(message.oneof_decl[1].name) == "X_a");
        }
        const FieldDescriptorProto* a = find_field(message, "a");
        CHECK(a != 0 && a->oneof_index == 1 && a->has_oneof_index);
    }

    // Two collisions grow two `X`s: `_a` taken, `X_a` taken -> `XX_a`.
    easypb_proto::ParsedProto two;
    easypb_proto::Diagnostic two_error;
    CHECK(easypb_proto::parse_proto("opt-two-collisions.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 _a = 1; int32 X_a = 2; optional int32 a = 3; }\n",
        two, two_error));
    if (!two.file.message_type.empty()) {
        const DescriptorProto& message = two.file.message_type[0];
        CHECK(message.oneof_decl.size() == 1);
        if (message.oneof_decl.size() == 1) {
            CHECK(text(message.oneof_decl[0].name) == "XX_a");
        }
    }

    // Synthetic oneofs are appended after every real oneof, whatever the
    // declaration order of the optional fields: real, `_a`, `_c`.
    easypb_proto::ParsedProto order;
    easypb_proto::Diagnostic order_error;
    CHECK(easypb_proto::parse_proto("opt-order.proto",
        "syntax = \"proto3\";\n"
        "message M { optional int32 a = 1; oneof real { int32 b = 2; } "
        "optional int32 c = 3; }\n", order, order_error));
    if (!order.file.message_type.empty()) {
        const DescriptorProto& message = order.file.message_type[0];
        CHECK(message.oneof_decl.size() == 3);
        if (message.oneof_decl.size() == 3) {
            CHECK(text(message.oneof_decl[0].name) == "real");
            CHECK(text(message.oneof_decl[1].name) == "_a");
            CHECK(text(message.oneof_decl[2].name) == "_c");
        }
        const FieldDescriptorProto* a = find_field(message, "a");
        const FieldDescriptorProto* c = find_field(message, "c");
        CHECK(a != 0 && a->oneof_index == 1);
        CHECK(c != 0 && c->oneof_index == 2);
    }

    // Proto2 is unaffected: no flag and no synthetic declaration.
    easypb_proto::ParsedProto proto2;
    easypb_proto::Diagnostic proto2_error;
    CHECK(easypb_proto::parse_proto("opt-proto2.proto",
        "syntax = \"proto2\";\n"
        "message M { optional int32 a = 1; }\n", proto2, proto2_error));
    if (!proto2.file.message_type.empty()) {
        const DescriptorProto& message = proto2.file.message_type[0];
        CHECK(message.oneof_decl.empty());
        CHECK(message.field.size() == 1);
        if (message.field.size() == 1) {
            CHECK(!message.field[0].has_proto3_optional);
            CHECK(!message.field[0].has_oneof_index);
        }
    }

    // An absent label, a repeated label and a map field never get presence.
    easypb_proto::ParsedProto implicit;
    easypb_proto::Diagnostic implicit_error;
    CHECK(easypb_proto::parse_proto("opt-implicit.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 a = 1; repeated int32 r = 2; "
        "map<string, int32> m = 3; }\n", implicit, implicit_error));
    if (!implicit.file.message_type.empty()) {
        const DescriptorProto& message = implicit.file.message_type[0];
        CHECK(message.oneof_decl.empty());
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            CHECK(!message.field[i].has_proto3_optional);
            CHECK(!message.field[i].has_oneof_index);
        }
    }

    // A nested message whose name collides with the synthetic declaration is
    // refused: protoc answers `"_a" is already defined in "p.M"` (measured
    // 2026-10-25), because a oneof is a symbol in the message scope.
    easypb_proto::ParsedProto nested;
    easypb_proto::Diagnostic nested_error;
    CHECK(!easypb_proto::parse_proto("opt-nested-collision.proto",
        "syntax = \"proto3\";\n"
        "package p;\n"
        "message M { message _a { int32 x = 1; } optional int32 a = 2; }\n",
        nested, nested_error));
    CHECK(nested_error.message.find("_a") != std::string::npos);
    CHECK(nested_error.message.find("collision") != std::string::npos);

    // Nested messages synthesize their own declarations independently.
    easypb_proto::ParsedProto nested_ok;
    easypb_proto::Diagnostic nested_ok_error;
    CHECK(easypb_proto::parse_proto("opt-nested.proto",
        "syntax = \"proto3\";\n"
        "message Outer { message Inner { optional int32 b = 1; } "
        "optional int32 a = 2; }\n", nested_ok, nested_ok_error));
    if (!nested_ok.file.message_type.empty()) {
        const DescriptorProto& outer = nested_ok.file.message_type[0];
        CHECK(outer.oneof_decl.size() == 1);
        if (outer.oneof_decl.size() == 1) {
            CHECK(text(outer.oneof_decl[0].name) == "_a");
        }
        CHECK(outer.nested_type.size() == 1);
        if (outer.nested_type.size() == 1) {
            CHECK(outer.nested_type[0].oneof_decl.size() == 1);
            if (outer.nested_type[0].oneof_decl.size() == 1) {
                CHECK(text(outer.nested_type[0].oneof_decl[0].name) == "_b");
            }
        }
    }
}

// D1-A: wire field 17 decodes as a bool beside the oneof index protoc writes
// for the synthetic declaration.
void test_decode_proto3_optional()
{
    // field 1 name "x"; field 9 oneof_index 0; field 17 proto3_optional true.
    const char encoded[] = {
        '\x0a', '\x01', 'x',
        '\x48', '\x00',
        '\x88', '\x01', '\x01',
    };
    FieldDescriptorProto field;
    decode(easypb::Decoder(encoded, sizeof(encoded)), field);
    CHECK(field.has_name && text(field.name) == "x");
    CHECK(field.has_oneof_index && field.oneof_index == 0);
    CHECK(field.has_proto3_optional);
    CHECK(field.proto3_optional);
}


void test_decode_oneof_index()
{
    const char encoded[] = { '\x0a', '\x01', 'x', '\x48', '\x00' };
    FieldDescriptorProto field;
    decode(easypb::Decoder(encoded, sizeof(encoded)), field);
    CHECK(field.has_name && text(field.name) == "x");
    CHECK(field.has_oneof_index && field.oneof_index == 0);
}

// D2-A: one source snippet per owner kind, so the family test can drive every
// row of the known-option table through the grammar. `name`/`value` are the
// option under test; the surrounding declaration is minimal and valid.
std::string option_owner_source(easypb_proto::OptionOwnerKind owner,
                                const std::string& name,
                                const std::string& value)
{
    const std::string option = name + " = " + value;
    switch (owner) {
        case easypb_proto::OPTION_OWNER_FILE:
            return "syntax = \"proto2\";\n" + std::string("option ") + option +
                   ";\nmessage Root { optional int32 x = 1; }\n";
        case easypb_proto::OPTION_OWNER_MESSAGE:
            return "syntax = \"proto2\";\n"
                   "message Root { option " + option + "; optional int32 x = 1; }\n";
        case easypb_proto::OPTION_OWNER_FIELD: {
            // The field kind that makes the row's own legality rule true:
            // `jstype` needs a 64-bit integer, `lazy` a message, `packed` a
            // repeated primitive.
            std::string declaration = "optional int32 x = 1";
            if (name == "jstype") declaration = "optional int64 x = 1";
            else if (name == "lazy" || name == "unverified_lazy") {
                declaration = "optional Inner x = 1";
            } else if (name == "packed") declaration = "repeated int32 x = 1";
            return "syntax = \"proto2\";\n"
                   "message Inner { optional int32 y = 1; }\n"
                   "message Root { " + declaration + " [" + option + "]; }\n";
        }
        case easypb_proto::OPTION_OWNER_ONEOF:
            return "syntax = \"proto2\";\n"
                   "message Root { oneof one { option " + option +
                   "; int32 x = 1; } }\n";
        case easypb_proto::OPTION_OWNER_ENUM: {
            // `allow_alias = true` is only legal beside a shared number
            // (CR-26), so it gets the aliased shape and every other enum
            // option keeps distinct numbers.
            const std::string values =
                name == "allow_alias" ? "A = 0; B = 0; " : "A = 0; B = 1; ";
            return "syntax = \"proto2\";\n"
                   "enum Root { option " + option + "; " + values + "}\n";
        }
        case easypb_proto::OPTION_OWNER_ENUM_VALUE:
            return "syntax = \"proto2\";\n"
                   "enum Root { A = 0 [" + option + "]; }\n";
        case easypb_proto::OPTION_OWNER_SERVICE:
            return "syntax = \"proto2\";\n"
                   "service Root { option " + option + "; }\n";
        case easypb_proto::OPTION_OWNER_METHOD:
            return "syntax = \"proto2\";\n"
                   "message Root { }\n"
                   "service S { rpc R (Root) returns (Root) { option " + option +
                   "; } }\n";
        case easypb_proto::OPTION_OWNER_EXTENSION_RANGE:
            return "syntax = \"proto2\";\n"
                   "message Root { extensions 100 to 199 [" + option + "]; }\n";
    }
    return std::string();
}

std::string wrong_option_value(easypb_proto::OptionConstantKind kind)
{
    switch (kind) {
        case easypb_proto::OPTION_CONSTANT_STRING: return "1";
        case easypb_proto::OPTION_CONSTANT_BOOL: return "\"x\"";
        case easypb_proto::OPTION_CONSTANT_IDENTIFIER: return "1";
        case easypb_proto::OPTION_CONSTANT_AGGREGATE: return "1";
        default: return "1";
    }
}

// D2-A: every row of the closed table is exercised in three directions - the
// accepted value kind, a wrong value kind, and an illegal placement - and the
// row counts pin the table to `descriptor.proto` 30.1's 47 known fields.
void test_known_option_table_covers_47_rows()
{
    using namespace easypb_proto;
    CHECK(known_option_count() == 47);

    std::size_t per_owner[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (std::size_t i = 0; i < known_option_count(); ++i) {
        ++per_owner[known_option_at(i).owner];
    }
    CHECK(per_owner[OPTION_OWNER_EXTENSION_RANGE] == 2);
    CHECK(per_owner[OPTION_OWNER_FILE] == 19);
    CHECK(per_owner[OPTION_OWNER_MESSAGE] == 5);
    CHECK(per_owner[OPTION_OWNER_FIELD] == 12);
    CHECK(per_owner[OPTION_OWNER_ONEOF] == 0);
    CHECK(per_owner[OPTION_OWNER_ENUM] == 3);
    CHECK(per_owner[OPTION_OWNER_ENUM_VALUE] == 3);
    CHECK(per_owner[OPTION_OWNER_SERVICE] == 1);
    CHECK(per_owner[OPTION_OWNER_METHOD] == 2);

    const OptionOwnerKind owners[] = {
        OPTION_OWNER_FILE,
        OPTION_OWNER_MESSAGE,
        OPTION_OWNER_FIELD,
        OPTION_OWNER_ONEOF,
        OPTION_OWNER_ENUM,
        OPTION_OWNER_ENUM_VALUE,
        OPTION_OWNER_SERVICE,
        OPTION_OWNER_METHOD,
        OPTION_OWNER_EXTENSION_RANGE,
    };

    for (std::size_t i = 0; i < known_option_count(); ++i) {
        const KnownOption& option = known_option_at(i);
        const std::string name(option.name);

        // The accepted value kind. `map_entry` is the one row the reference
        // compiler refuses in source text whatever the value.
        std::string accepted = "\"x\"";
        if (option.value_kind == OPTION_CONSTANT_BOOL) accepted = "true";
        else if (option.value_kind == OPTION_CONSTANT_IDENTIFIER) {
            const char* first = known_option_first_enum_value(option);
            CHECK(first != 0);
            accepted = first == 0 ? std::string() : std::string(first);
        } else if (option.value_kind == OPTION_CONSTANT_AGGREGATE) {
            accepted = "{ }";
        }

        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "known-option.proto",
            option_owner_source(option.owner, name, accepted), parsed, error);
        if (option.refuse_reason != 0) {
            CHECK(!ok);
            CHECK(error.message.find(option.refuse_reason) != std::string::npos);
        } else if (!ok) {
            std::cerr << name << " (owner " << option.owner
                      << ") was refused: " << error.message << '\n';
            CHECK(ok);
        }

        // A wrong value kind is refused, except for the always-refused row.
        if (option.refuse_reason == 0) {
            easypb_proto::ParsedProto wrong;
            easypb_proto::Diagnostic wrong_error;
            CHECK(!easypb_proto::parse_proto(
                "known-option-wrong.proto",
                option_owner_source(option.owner, name,
                                    wrong_option_value(option.value_kind)),
                wrong, wrong_error));
        }

        // The same name under an owner whose `*Options` message has no such
        // field is an unknown option. `oneof` has no known fields at all, so
        // a placement always exists for a row owned elsewhere.
        OptionOwnerKind other = OPTION_OWNER_ONEOF;
        if (option.owner == OPTION_OWNER_ONEOF) other = OPTION_OWNER_FILE;
        if (find_known_option(other, name) != 0) continue;
        easypb_proto::ParsedProto misplaced;
        easypb_proto::Diagnostic misplaced_error;
        CHECK(!easypb_proto::parse_proto(
            "known-option-placement.proto",
            option_owner_source(other, name, accepted), misplaced,
            misplaced_error));
        CHECK(misplaced_error.message.find("unknown") != std::string::npos);
    }
}

// D2-A: the measured G2 examples are refusals on the source path, their legal
// counterparts are accepted, an unknown unparenthesized name is refused, and
// a parenthesized custom option stays opaque.
void test_known_option_measured_cases()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    // The five measured refusals (protoc 30.1, rc 1; codegen used to accept).
    CHECK(!easypb_proto::parse_proto("g2-java.proto",
        "syntax = \"proto3\";\noption java_package = 7;\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-optimize.proto",
        "syntax = \"proto3\";\noption optimize_for = 7;\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-jstype.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 x = 1 [jstype = JS_STRING]; }\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-lazy.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 x = 1 [lazy = true]; }\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-unverified.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 x = 1 [unverified_lazy = true]; }\n", parsed, error));

    // The legal counterparts.
    CHECK(easypb_proto::parse_proto("g2-java-ok.proto",
        "syntax = \"proto3\";\noption java_package = \"x\";\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-optimize-ok.proto",
        "syntax = \"proto3\";\noption optimize_for = SPEED;\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-jstype-ok.proto",
        "syntax = \"proto3\";\n"
        "message M { int64 x = 1 [jstype = JS_STRING]; }\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-lazy-ok.proto",
        "syntax = \"proto3\";\n"
        "message N { int32 y = 1; }\n"
        "message M { N x = 1 [lazy = true]; }\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-packed-ok.proto",
        "syntax = \"proto3\";\n"
        "message M { repeated int32 x = 1 [packed = true]; }\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-alias-ok.proto",
        "syntax = \"proto2\";\n"
        "enum E { option allow_alias = true; A = 1; B = 1; }\n", parsed, error));

    // An unknown unparenthesized name is refused with the option name in the
    // diagnostic; a parenthesized custom option is opaque and accepted.
    easypb_proto::ParsedProto unknown;
    easypb_proto::Diagnostic unknown_error;
    CHECK(!easypb_proto::parse_proto("g2-unknown.proto",
        "syntax = \"proto3\";\noption foo = 1;\n", unknown, unknown_error));
    CHECK(unknown_error.message.find("foo") != std::string::npos);
    CHECK(unknown_error.message.find("unknown") != std::string::npos);
    CHECK(easypb_proto::parse_proto("g2-custom.proto",
        "syntax = \"proto3\";\noption (foo) = 1;\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-custom-aggregate.proto",
        "syntax = \"proto3\";\noption (foo) = { a: 1 };\n", parsed, error));

    // Duplicate, placement, enum-value and owner mismatches.
    CHECK(!easypb_proto::parse_proto("g2-dup.proto",
        "syntax = \"proto3\";\n"
        "option java_package = \"a\";\noption java_package = \"b\";\n",
        parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-misplaced.proto",
        "syntax = \"proto3\";\n"
        "message M { option java_package = \"x\"; }\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-oneof.proto",
        "syntax = \"proto3\";\n"
        "message M { oneof o { option deprecated = true; int32 a = 1; } }\n",
        parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-enum-value.proto",
        "syntax = \"proto2\";\n"
        "enum E { A = 1 [jstype = JS_STRING]; }\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-enum-value-ok.proto",
        "syntax = \"proto2\";\n"
        "enum E { A = 1 [deprecated = true]; }\n", parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-map-entry.proto",
        "syntax = \"proto3\";\nmessage M { option map_entry = true; }\n",
        parsed, error));
    CHECK(!easypb_proto::parse_proto("g2-optimize-value.proto",
        "syntax = \"proto3\";\noption optimize_for = FOO;\n", parsed, error));

    // Owner-specific accept controls.
    CHECK(easypb_proto::parse_proto("g2-service.proto",
        "syntax = \"proto3\";\nservice S { option deprecated = true; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("g2-method.proto",
        "syntax = \"proto3\";\n"
        "message M { }\n"
        "service S { rpc R (M) returns (M) { "
        "option idempotency_level = NO_SIDE_EFFECTS; } }\n", parsed, error));
    CHECK(easypb_proto::parse_proto("g2-ext-range.proto",
        "syntax = \"proto2\";\n"
        "message M { extensions 100 to 199 [verification = UNVERIFIED]; }\n",
        parsed, error));
    CHECK(easypb_proto::parse_proto("g2-ext-declaration.proto",
        "syntax = \"proto2\";\n"
        "message M { extensions 100 to 199 "
        "[declaration = { number: 100, full_name: \".p.M.f\", type: \"int32\" }]; }\n",
        parsed, error));
    // The pair, not either value alone, is invalid: a non-empty declaration
    // list forbids `verification = UNVERIFIED` (measured with protoc 30.1).
    CHECK(!easypb_proto::parse_proto("g2-ext-unverified.proto",
        "syntax = \"proto2\";\n"
        "message M { extensions 100 to 199 "
        "[declaration = { number: 100, full_name: \".p.M.f\", type: \"int32\" }, "
        "verification = UNVERIFIED]; }\n", parsed, error));
    CHECK(error.message.find("UNVERIFIED") != std::string::npos);
}

// D2-A: the value-dependent field rules. `jstype` is 64-bit-only; `lazy` and
// `unverified_lazy` are message-only (a named enum is decided after
// resolution); `ctype` and `weak` have no caretaker rule in protoc 30.1
// (measured 2026-10-25: both are accepted on an int32 field), so this project
// does not invent one.
void test_known_option_field_kind_rules()
{
    const char* accepted[] = {
        "message M { int64 x = 1 [jstype = JS_STRING]; }",
        "message M { uint64 x = 1 [jstype = JS_STRING]; }",
        "message M { sint64 x = 1 [jstype = JS_NUMBER]; }",
        "message M { fixed64 x = 1 [jstype = JS_NORMAL]; }",
        "message M { sfixed64 x = 1 [jstype = JS_STRING]; }",
        "message M { int32 x = 1 [jstype = JS_NORMAL]; }",
        "message M { string x = 1 [jstype = JS_NORMAL]; }",
        "message M { int32 x = 1 [lazy = false]; }",
        "message M { string x = 1 [lazy = false]; }",
        "message M { int32 x = 1 [unverified_lazy = false]; }",
        "message M { map<string, string> t = 1 [packed = false]; }",
        "message M { int32 x = 1 [ctype = CORD]; }",
        "message M { int32 x = 1 [weak = true]; }",
        "message M { repeated int32 x = 1 [packed = true]; }",
        "message N { int32 y = 1; } message M { N x = 1 [lazy = true]; }",
        "message N { int32 y = 1; } message M { N x = 1 [unverified_lazy = true]; }",
    };
    for (std::size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "option-field-kind.proto",
            std::string("syntax = \"proto3\";\n") + accepted[i], parsed, error);
        if (!ok) std::cerr << accepted[i] << ": " << error.message << '\n';
        CHECK(ok);
    }

    const char* refused[] = {
        "message M { int32 x = 1 [jstype = JS_STRING]; }",
        "message M { string x = 1 [jstype = JS_STRING]; }",
        "message M { repeated int32 x = 1 [packed = true]; } "
        "message P { int32 y = 1 [packed = true]; }",
        "message M { repeated string x = 1 [packed = true]; }",
        "enum E { A = 0; } message M { E x = 1 [lazy = true]; }",
        "enum E { A = 0; } message M { E x = 1 [unverified_lazy = true]; }",
    };
    for (std::size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "option-field-kind-bad.proto",
            std::string("syntax = \"proto3\";\n") + refused[i], parsed, error);
        if (ok) std::cerr << "accepted: " << refused[i] << '\n';
        CHECK(!ok);
    }

    // R09 (U-42): the message-only `lazy` rule is not a table rule. The
    // parser keeps the non-default flag in the field provenance and the shared
    // field pass answers it, so a scalar is still refused when type resolution
    // is deferred - a table-side branch would be unreachable here and proves
    // nothing.
    easypb_proto::ParsedProto deferred;
    easypb_proto::Diagnostic deferred_error;
    easypb_proto::ParseOptions deferred_options;
    deferred_options.defer_type_resolution = true;
    CHECK(!easypb_proto::parse_proto("g2-lazy-deferred.proto",
        "syntax = \"proto3\";\n"
        "message M { int32 x = 1 [lazy = true]; }\n",
        deferred, deferred_error, deferred_options));
    CHECK(deferred_error.message.find("submessage fields") != std::string::npos);
}

// R14 (U-49): an enum value's bracket list is validated by the known-option
// table alone. `default`, `packed` and `json_name` are not fields of
// `EnumValueOptions`, so each is refused as unknown; `(easypb.cpp).type` is a
// field option and refused by the scope rule (U-54). The parser's former
// post-check could never fire, and this family pins every verdict and its
// sentence so its removal cannot change acceptance or wording.
void test_enum_value_bracket_options_are_table_rules()
{
    const char* accepted[] = {
        "enum E { A = 0 [deprecated = true]; }",
        "enum E { A = 0 [debug_redact = true]; }",
        "enum E { A = 0 [feature_support = { }]; }",
    };
    for (std::size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        const bool ok = easypb_proto::parse_proto(
            "enum-value-option.proto",
            std::string("syntax = \"proto2\";\n") + accepted[i], parsed, error);
        if (!ok) std::cerr << accepted[i] << ": " << error.message << '\n';
        CHECK(ok);
    }

    const char* refused[] = {
        "enum E { A = 0 [default = 1]; }",
        "enum E { A = 0 [packed = true]; }",
        "enum E { A = 0 [json_name = \"x\"]; }",
    };
    for (std::size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto(
            "enum-value-option-bad.proto",
            std::string("syntax = \"proto2\";\n") + refused[i], parsed, error));
        CHECK(error.message.find("unknown") != std::string::npos);
    }

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("enum-value-cpp-type.proto",
        "syntax = \"proto2\";\n"
        "enum E { A = 0 [(easypb.cpp).type = \"::x\"]; }\n", parsed, error));
    CHECK(error.message.find("cannot be set at enum value scope") !=
          std::string::npos);
}

// R14 (U-50): a reserved name that is not an identifier is accepted with a
// warning, as protoc 30.1 does (parser.cc:1767-1777). The warning is per name
// and applies to the message and enum spellings alike; a legal identifier is
// silent.
void test_reserved_non_identifier_warns()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(easypb_proto::parse_proto("reserved-non-identifier.proto",
        "syntax = \"proto2\";\n"
        "message M { reserved \"foo bar\", \"ok\"; }\n"
        "enum E { reserved \"x y\"; A = 0; }\n", parsed, error));
    CHECK(parsed.warnings.size() == 2);
    if (parsed.warnings.size() == 2) {
        CHECK(parsed.warnings[0].warning);
        CHECK(parsed.warnings[0].message ==
              "Reserved name \"foo bar\" is not a valid identifier.");
        CHECK(parsed.warnings[0].location.line == 2);
        CHECK(parsed.warnings[0].location.column == 22);
        CHECK(parsed.warnings[1].message ==
              "Reserved name \"x y\" is not a valid identifier.");
        CHECK(parsed.warnings[1].location.line == 3);
        CHECK(parsed.warnings[1].location.column == 19);
    }

    // The accept control is the same declaration with identifier names only.
    easypb_proto::ParsedProto control;
    easypb_proto::Diagnostic control_error;
    CHECK(easypb_proto::parse_proto("reserved-identifiers.proto",
        "syntax = \"proto2\";\n"
        "message M { reserved \"foo\", \"ok\"; }\n", control, control_error));
    CHECK(control.warnings.empty());
}

// R14 (U-51): protoc appends the allow_alias hint to the folded-name sentence
// whether or not the option is set. The rule lives in the shared enum walk the
// linker runs, so the test parses (the standalone parser does not run that
// walk) and asks the walk directly; the second case keeps `allow_alias = true`
// legal (CR-26 needs a shared number) while two other names still fold.
void test_folded_enum_name_message_always_has_the_alias_suffix()
{
    const std::string suffix =
        " (If you are using allow_alias, please assign the same number to "
        "each enum value name.)";

    easypb_proto::ParsedProto folded;
    easypb_proto::Diagnostic parse_error;
    CHECK(easypb_proto::parse_proto("folded-name.proto",
        "syntax = \"proto2\";\n"
        "enum E { FOO = 0; foo = 1; }\n", folded, parse_error));
    easypb_schema::Diagnostic enum_error;
    CHECK(!easypb_schema::validate_enum_value_numbers(folded, enum_error));
    CHECK(enum_error.message.find("same name as") != std::string::npos);
    CHECK(enum_error.message.find(suffix) != std::string::npos);

    easypb_proto::ParsedProto aliased;
    CHECK(easypb_proto::parse_proto("folded-name-alias.proto",
        "syntax = \"proto2\";\n"
        "enum AliasDiff { option allow_alias = true;\n"
        "  ALIAS_DIFF_FOO = 0; ALIAS_DIFF_BAR = 0; FOO = 1; }\n",
        aliased, parse_error));
    easypb_schema::Diagnostic aliased_error;
    CHECK(!easypb_schema::validate_enum_value_numbers(aliased, aliased_error));
    CHECK(aliased_error.message.find("same name as") != std::string::npos);
    CHECK(aliased_error.message.find(suffix) != std::string::npos);
}

// R14 (U-62): the value-kind rule is asked before the row's own refusal, so a
// message-shaped (aggregate) or integer value names the kind defect; a
// well-typed `true` still gets the row's `map_entry` sentence.
void test_map_entry_refusal_names_the_value_kind_first()
{
    const char* wrong_kind[] = {
        "message M { option map_entry = { }; }",
        "message M { option map_entry = 5; }",
    };
    for (std::size_t i = 0; i < sizeof(wrong_kind) / sizeof(wrong_kind[0]); ++i) {
        easypb_proto::ParsedProto parsed;
        easypb_proto::Diagnostic error;
        CHECK(!easypb_proto::parse_proto(
            "map-entry-kind.proto",
            std::string("syntax = \"proto3\";\n") + wrong_kind[i], parsed,
            error));
        CHECK(error.message.find(
                  "Value must be identifier for boolean option \"map_entry\"") !=
              std::string::npos);
        CHECK(error.message.find("map_entry should not be set explicitly") ==
              std::string::npos);
    }

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("map-entry-well-typed.proto",
        "syntax = \"proto3\";\nmessage M { option map_entry = true; }\n",
        parsed, error));
    CHECK(error.message.find("map_entry should not be set explicitly") !=
          std::string::npos);
    CHECK(error.message.find("Value must be identifier") == std::string::npos);
}

// R08: the option *value* grammar. A value is one identifier (U-23), a leading
// `-` before an aggregate is ignored (U-24), a number carries at most one
// decimal point or exponent (U-26), and the aggregate balancer is checked
// (U-47).
void test_option_value_grammar()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;

    // U-23: one identifier is the whole value; a dotted path stops at the dot,
    // so the list or statement terminator is missing.
    CHECK(easypb_proto::parse_proto("value-identifier.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1 [(myopt) = foo]; }",
        parsed, error));
    CHECK(!easypb_proto::parse_proto("value-dotted.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1 [(myopt) = foo.bar]; }",
        parsed, error));
    CHECK(error.message.find("expected ']'") != std::string::npos);
    CHECK(!easypb_proto::parse_proto("value-dotted-statement.proto",
        "syntax = \"proto2\"; option (myopt) = foo.bar;", parsed, error));
    CHECK(error.message.find("expected ';'") != std::string::npos);

    // U-24: `-` before an aggregate is ignored, in a statement and in a field
    // bracket list; `+` is not a value sign and stays refused.
    CHECK(easypb_proto::parse_proto("value-negative-aggregate.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1; } "
        "option (myopt) = -{ a: 1 };", parsed, error));
    CHECK(easypb_proto::parse_proto("value-negative-aggregate-field.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1 "
        "[(myopt) = -{ a: 1 }]; }", parsed, error));
    CHECK(!easypb_proto::parse_proto("value-positive-aggregate.proto",
        "syntax = \"proto2\"; message M { optional int32 a = 1; } "
        "option (myopt) = +{ a: 1 };", parsed, error));
    CHECK(error.message.find("Expected option value") != std::string::npos);
    // A known aggregate-valued option takes the same parser path: protoc
    // accepts the signed body and validates the declaration inside it.
    CHECK(easypb_proto::parse_proto("value-known-aggregate.proto",
        "syntax = \"proto2\"; message M { extensions 100 to 199 "
        "[declaration = -{ number: 100, full_name: \".p.M.f\", type: \"int32\" }]; }",
        parsed, error));

    // U-26: a second decimal point inside an aggregate is a tokenizer error; a
    // trailing dot, an exponent and a leading dot are still single tokens.
    CHECK(!easypb_proto::parse_proto("value-double-dot.proto",
        "syntax = \"proto2\"; option (myopt) = { v: 1.2.3 };", parsed, error));
    CHECK(error.message.find("another one") != std::string::npos);
    CHECK(easypb_proto::parse_proto("value-number-forms.proto",
        "syntax = \"proto2\"; option (a) = { v: 1. }; "
        "option (b) = { v: 1e5 }; option (c) = { v: .5 };", parsed, error));

    // U-47: a deep but balanced aggregate is accepted (the counter neither
    // goes negative nor wraps), and an unterminated one still reports at the
    // end of the input where the balance is decided.
    const int depth = 4096;
    std::string deep = "syntax = \"proto2\"; option (myopt) = ";
    deep.append(static_cast<std::size_t>(depth), '{');
    deep += "a: 1";
    deep.append(static_cast<std::size_t>(depth), '}');
    deep += ";";
    CHECK(easypb_proto::parse_proto("value-deep-aggregate.proto", deep, parsed, error));

    const std::string unbalanced =
        "syntax = \"proto2\"; option (myopt) = { a: { b: 1 };";
    CHECK(!easypb_proto::parse_proto("value-unbalanced.proto", unbalanced, parsed, error));
    CHECK(error.message.find("unterminated aggregate option value") != std::string::npos);
    CHECK(error.location.line == 1);
    CHECK(error.location.column == static_cast<int>(unbalanced.size()) + 1);
}

} // namespace

int main()
{
    test_complex_proto3();
    test_proto2_defaults_and_literals();
    test_empty_syntax_statement_is_refused();
    test_source_refusals_carry_their_descriptor_codes();
    test_bom_is_consumed_by_the_lexer();
    test_float_defaults_are_canonicalized();
    test_bytes_upper_hex_escape_is_decoded();
    test_leading_plus_is_refused();
    test_statement_and_option_grammar_table();
    test_json_name_option_is_retained();
    test_rejects_proto3_required();
    test_rejects_invalid_field_numbers();
    test_map_keyword_can_be_a_type_name();
    test_rejects_invalid_map_key();
    test_map_option_rejections_point_at_the_option();
    test_reserved_and_extension_ranges_are_retained();
    test_rejects_wrong_default_token_kind();
    test_64_bit_default_ranges();
    test_numeric_literal_forms();
    test_numeric_default_spellings_are_canonical();
    test_lexer_diagnostics();
    test_tab_columns_expand_to_tab_stops();
    test_lexer_escape_table();
    test_lexer_lexeme_table();
    test_lexer_comment_and_control_rules();
    test_edition_statement_is_refused_by_name();
    test_rejects_reserved_conflicts();
    test_rejects_extension_range_conflicts();
    test_reserved_range_reaches_the_int32_bound();
    test_message_set_extension_bounds();
    test_rejects_reserved_enum_values();
    test_rejects_enum_scope_name_collisions();
    test_rejects_invalid_enum_defaults();
    test_keyword_enum_defaults();
    test_oneof_rejects_empty_statement();
    test_extend_requires_a_field();
    test_proto3_extendee_is_resolved_before_the_allowlist();
    test_rejects_empty_enums();
    test_post_body_refusals_point_inside_the_declaration();
    test_rejects_duplicate_oneof_names();
    test_rejects_an_empty_oneof_body();
    test_records_allow_alias_in_the_model();
    test_buffer_api_owns_result_strings();
    test_custom_option_name_parts_are_consumed();
    test_easypb_cpp_field_options_are_retained();
    test_cpp_type_option_is_field_only();
    test_malformed_custom_option_name_parts_are_rejected();
    test_service_rpc_syntax_is_consumed();
    test_rpc_streaming_presence_flags();
    test_service_is_rejected_inside_message();
    test_rejects_service_scope_name_collisions();
    test_rejects_malformed_rpc_syntax();
    test_decode_all_field_descriptor_members();
    test_decode_complete_descriptor_set();
    test_decode_oneof_index();
    test_decode_file_syntax();
    test_imports_populate_descriptor_dependencies();
    test_package_declaration_location_is_recorded();
    test_service_names_are_retained();
    test_deferred_leaves_imported_types_unresolved();
    test_standalone_keeps_unresolved_placeholder_and_warnings();
    test_deferred_defers_kind_dependent_checks();
    test_deferred_ignores_shadowed_local_kind();
    test_deferred_nested_named_fields_stay_silent();
    test_packed_requires_repeated_label();
    test_unresolved_default_is_deferred_not_rejected();
    test_unresolved_warnings_are_per_field();
    test_parsed_metadata_survives_source_destruction();
    test_proto3_optional_synthetic_oneofs();
    test_decode_proto3_optional();
    test_known_option_table_covers_47_rows();
    test_known_option_measured_cases();
    test_known_option_field_kind_rules();
    test_enum_value_bracket_options_are_table_rules();
    test_reserved_non_identifier_warns();
    test_folded_enum_name_message_always_has_the_alias_suffix();
    test_map_entry_refusal_names_the_value_kind_first();
    test_option_value_grammar();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all parser tests passed\n";
    return EXIT_SUCCESS;
}
