#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "proto_parser.hpp"

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

    // CR-18's overlap: the integer token `08` is not octal and protoc refuses
    // it on the float path too (its ConsumeNumber reads an INTEGER token with
    // the octal parser first). The parser's refusal happens before any
    // canonicalization, so the value never silently becomes 8.
    CHECK(!easypb_proto::parse_proto("bad-octal-float.proto",
        "message M { optional float x = 1 [default = 08]; }", parsed, error));
    CHECK(error.message.find("Integer out of range") != std::string::npos);
}

void test_lexer_diagnostics()
{
    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    CHECK(!easypb_proto::parse_proto("bad-octal.proto",
        "message M { optional int32 x = 1 [default = 08]; }", parsed, error));
    CHECK(error.message.find("signed 32-bit") != std::string::npos);

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

// D-32 (fix round r16): the source half of the empty-enum rule. The parser
// refuses the shape as text, which is why the descriptor-origin half is what
// the linker owns; the option-only body is still an empty list, and that row is
// the parity control for the descriptor-origin `allow_alias` case - protoc
// refuses an empty enum that writes the option as well, so the option is not a
// licence on either origin.
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
    CHECK(option_error.message.find("at least one value") != std::string::npos);
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
// without it. The three states matter and are asserted separately: absent (the
// parser's own duplicate-number refusal is the only guard), present and true
// (an alias the linker must accept - the end-to-end fixture is
// `tests/codegen/enums/data/enum_codegen.proto`, whose `STARTED = 1` and
// `ACTIVE = 1` sit under the option), and present and false (not a licence).
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
    CHECK(easypb_proto::parse_proto("false.proto",
        "syntax=\"proto2\"; enum E { option allow_alias = false; A = 1; B = 2; }",
        explicit_false, false_error));
    CHECK(explicit_false.file.enum_type.size() == 1);
    if (explicit_false.file.enum_type.size() == 1) {
        CHECK(explicit_false.file.enum_type[0].options.has_allow_alias);
        CHECK(!explicit_false.file.enum_type[0].options.allow_alias);
    }
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


void test_full_custom_option_name_parts_are_consumed()
{
    const char* proto2_source =
        "syntax = \"proto2\";\n"
        "option file_scope.(vendor.file).(.absolute.path) = true;\n"
        "option (legacy.option).leaf = true;\n"
        "enum E {\n"
        "  option enum_scope.(vendor.enum) = true;\n"
        "  option allow_alias = true;\n"
        "  ZERO = 0 [value_scope.(vendor.value) = 1];\n"
        "  ALIAS = 0;\n"
        "}\n"
        "message M {\n"
        "  option (.vendor.message).leaf = 1;\n"
        "  extensions 100 to 199 [range_scope.(vendor.range) = true];\n"
        "  repeated int32 values = 1 "
        "      [field_scope.(vendor.field).(.nested.extension) = 7, packed = true];\n"
        "  oneof choice {\n"
        "    option oneof_scope.(vendor.oneof) = true;\n"
        "    string text = 2 [oneof_field_scope.(vendor.field) = true];\n"
        "  }\n"
        "}\n"
        "service S {\n"
        "  option service_scope.(vendor.service) = true;\n"
        "  rpc R(M) returns (M) {\n"
        "    option method_scope.(vendor.method) = true;\n"
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
        "option proto3_scope.(vendor.file).tail = true;\n"
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

void test_decode_oneof_index()
{
    const char encoded[] = { '\x0a', '\x01', 'x', '\x48', '\x00' };
    FieldDescriptorProto field;
    decode(easypb::Decoder(encoded, sizeof(encoded)), field);
    CHECK(field.has_name && text(field.name) == "x");
    CHECK(field.has_oneof_index && field.oneof_index == 0);
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
    test_json_name_option_is_retained();
    test_rejects_proto3_required();
    test_rejects_invalid_field_numbers();
    test_map_keyword_can_be_a_type_name();
    test_rejects_invalid_map_key();
    test_map_option_rejections_point_at_the_option();
    test_rejects_wrong_default_token_kind();
    test_64_bit_default_ranges();
    test_numeric_literal_forms();
    test_numeric_default_spellings_are_canonical();
    test_lexer_diagnostics();
    test_edition_statement_is_refused_by_name();
    test_rejects_reserved_conflicts();
    test_rejects_extension_range_conflicts();
    test_rejects_reserved_enum_values();
    test_rejects_enum_scope_name_collisions();
    test_rejects_invalid_enum_defaults();
    test_rejects_empty_enums();
    test_rejects_duplicate_oneof_names();
    test_rejects_an_empty_oneof_body();
    test_records_allow_alias_in_the_model();
    test_buffer_api_owns_result_strings();
    test_full_custom_option_name_parts_are_consumed();
    test_easypb_cpp_field_options_are_retained();
    test_malformed_custom_option_name_parts_are_rejected();
    test_service_rpc_syntax_is_consumed();
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all parser tests passed\n";
    return EXIT_SUCCESS;
}
