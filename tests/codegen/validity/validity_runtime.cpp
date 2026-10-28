// The validity lane's runtime check: the default values the schema names,
// byte for byte.
//
// Two of the defects this lane covers are SILENT: a header that says
// `::std::string s = "a\b";` compiles cleanly and holds a backspace, and one
// that says `= "a\0b";` compiles cleanly and holds one byte. Only a runtime
// check of the bytes catches them, so this program is the proof, not the
// compile.
//
// The headers are the descriptor-path ones, because the descriptor path is
// the one every build profile has. The driver compares the source path's
// header with the descriptor path's byte for byte, so the values checked
// here are the values both paths produce.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "defaults-backslash.pb.hpp"
#include "defaults-bytes-upper-hex.pb.hpp"
#include "defaults-float-keywords.pb.hpp"
#include "defaults-float-overflow.pb.hpp"
#include "defaults-float-subnormal.pb.hpp"
#include "defaults-inf.pb.hpp"
#include "defaults-nan.pb.hpp"
#include "defaults-newline.pb.hpp"
#include "defaults-nul.pb.hpp"
#include "defaults-quote-byte.pb.hpp"
#include "defaults-quote.pb.hpp"
#include "defaults-spellings.pb.hpp"
#include "defaults-trigraph.pb.hpp"
#include "empty-syntax-descriptor.pb.hpp"
#include "enum-alias-same-number.pb.hpp"
#include "json-name-custom-vs-default-proto2.pb.hpp"
#include "json-name-proto2-warning.pb.hpp"
#include "json-names-distinct.pb.hpp"
#include "leading-space-control.pb.hpp"
#include "literals-descriptor-octal.pb.hpp"
#include "literals-escapes.pb.hpp"
#include "literals-lexemes.pb.hpp"
#include "packages-deep-101.pb.hpp"
#include "plus-controls.pb.hpp"
#include "plus-descriptor-controls.pb.hpp"
#include "reserved-range-end.pb.hpp"
#include "scope-easypb.pb.hpp"
#include "scope-nested-reserved.pb.hpp"
#include "p3opt-ok.pb.hpp"
#include "oneof-contiguous.pb.hpp"
#include "message-set-empty.pb.hpp"

// W-38's accepted boundary: the 101-component package compiles and its
// message can be named.
namespace deep_package = c001::c002::c003::c004::c005::c006::c007::c008::c009::c010::c011::c012::c013::c014::c015::c016::c017::c018::c019::c020::c021::c022::c023::c024::c025::c026::c027::c028::c029::c030::c031::c032::c033::c034::c035::c036::c037::c038::c039::c040::c041::c042::c043::c044::c045::c046::c047::c048::c049::c050::c051::c052::c053::c054::c055::c056::c057::c058::c059::c060::c061::c062::c063::c064::c065::c066::c067::c068::c069::c070::c071::c072::c073::c074::c075::c076::c077::c078::c079::c080::c081::c082::c083::c084::c085::c086::c087::c088::c089::c090::c091::c092::c093::c094::c095::c096::c097::c098::c099::c100::c101;

namespace {

int failures = 0;

void report(const char* what, bool ok)
{
    std::printf("%-14s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

// The bytes of a default value, spelled the way the schema spells them.
bool is_bytes(const std::string& value, const char* expected, std::size_t size)
{
    return value.size() == size &&
           std::memcmp(value.data(), expected, size) == 0;
}

} // namespace

int main()
{
    // W-03's numeric-only control: whitespace before a source token is not
    // part of the value, and a string default keeps its leading space.
    {
        leadingspace::LeadingSpaceControl value;
        report("leadspace", std::isinf(value.positive_infinity) &&
                            value.positive_infinity > 0.0f);
        const char expected[] = { ' ', 'x' };
        report("leadstr", is_bytes(value.spaced, expected, sizeof expected));
    }
    // A quote inside a string default: three bytes, the quote among them.
    {
        QuoteDefaults value;
        const char expected[] = { 'a', '"', 'b' };
        report("quote", is_bytes(value.quoted, expected, sizeof expected));
    }
    // A backslash inside a string default: three bytes, the backslash among
    // them - and NOT a backspace.
    {
        BackslashDefaults value;
        const char expected[] = { 'a', '\\', 'b' };
        report("backslash", is_bytes(value.backslash, expected, sizeof expected));
    }
    // A newline inside a string default: three bytes, the newline among them.
    {
        NewlineDefaults value;
        const char expected[] = { 'a', '\n', 'b' };
        report("newline", is_bytes(value.newline, expected, sizeof expected));
    }
    // An embedded NUL: three bytes, the NUL among them - the length is what
    // keeps the value whole.
    {
        NulDefaults value;
        const char expected[] = { 'a', '\0', 'b' };
        report("nul", is_bytes(value.nul, expected, sizeof expected));
        report("nulbytes", is_bytes(value.nulbytes, expected, sizeof expected));
    }
    // A quote inside a bytes default: the stored text is C-escaped, and the
    // value is the three bytes it decodes to.
    {
        QuoteByteDefaults value;
        const char expected[] = { 'a', '"', 'b' };
        report("quotebyte", is_bytes(value.quoted, expected, sizeof expected));
    }
    // A trigraph opener: the header is compiled with -std=c++11, where the
    // replacement happens in translation phase 1 - before the backslash
    // escape is read - so the literal has to break every "??X" pair.
    {
        TrigraphDefaults value;
        const char assignment[] = { 'a', '?', '?', '=', 'b' };
        report("trigraph =", is_bytes(value.assignment, assignment,
                                      sizeof assignment));
        const char slash[] = { 'a', '?', '?', '/', 'b' };
        report("trigraph /", is_bytes(value.slash, slash, sizeof slash));
        const char less[] = { 'a', '?', '?', '<', 'b' };
        report("trigraph <", is_bytes(value.less, less, sizeof less));
    }
    // W-04: the canonicalized ERANGE family. The header text is the value
    // protoc stores, and the runtime distinguishes the two silent cases:
    // underflow to negative zero and a finite subnormal.
    {
        FloatOverflowDefaults value;
        report("overflow", std::isinf(value.overflow) && value.overflow > 0);
        report("underflow", value.underflow == 0 &&
                               !std::signbit(value.underflow));
        report("neg underflow", value.negative_underflow == 0 &&
                                   std::signbit(value.negative_underflow));
        report("subnormal", std::fpclassify(value.subnormal) == FP_SUBNORMAL);
    }
    // U-18: a float default that narrows to a finite subnormal. The header
    // carries protoc's nine-digit spelling; the runtime proves the literal is
    // the subnormal itself, not zero and not the next normal float.
    {
        subnormal::FloatSubnormalDefaults value;
        report("subnormal f", std::fpclassify(value.tiny) == FP_SUBNORMAL);
        report("subnormal fmin", value.minimum ==
                                     std::numeric_limits<float>::denorm_min());
        report("subnormal d",
               std::fpclassify(value.tiny_double) == FP_SUBNORMAL);
    }
    // U-56: a reserved declaration may name 536870912. The two empty messages
    // carry no fields, so the header compiling is the whole proof.
    {
        reservedend::BigReservedValue value;
        reservedend::BigReservedRange range;
        (void)value;
        (void)range;
    }
    // R1 / CR-01 and CR-17: the numeric default spelling family. An integer
    // token on a float field is read with the octal/hex parser (so `010` is
    // eight, not ten), and an integral spelling is stored as the decimal
    // value protoc stores. The descriptor twin is protoc's own encoding of
    // the same source, so the byte-identical headers prove the values match
    // protoc's `--descriptor_set_out`.
    {
        DefaultSpellings value;
        report("f octal", value.f_octal == 8.0f);
        report("d octal", value.d_octal == 8.0);
        report("f hex", value.f_hex == 16.0f);
        report("d hex", value.d_hex == 16.0);
        report("f -octal", value.f_neg_octal == -8.0f);
        report("i32 octal", value.i32_octal == 8);
        report("i32 hex", value.i32_hex == 16);
        report("i32 -0", value.i32_neg_zero == 0);
        report("i64 octal", value.i64_octal == 63);
        report("i64 min", value.i64_min ==
                              std::numeric_limits<std::int64_t>::min());
        report("i64 max", value.i64_max ==
                              std::numeric_limits<std::int64_t>::max());
        report("u64 max", value.u64_max ==
                              std::numeric_limits<std::uint64_t>::max());
        report("u64 hexmax", value.u64_hex_max ==
                                 std::numeric_limits<std::uint64_t>::max());
        report("u64 octal", value.u64_octal == 63);
    }
    // CR-02: the descriptor-only spellings protoc accepts (`INF`, `Infinity`,
    // `NAN`, `nan(payload)`) reach the header as numeric_limits calls, so the
    // member holds the value and the header compiles.
    {
        FloatKeywordSpellings value;
        report("INF", std::isinf(value.f_upper_inf) &&
                          value.f_upper_inf > 0);
        report("Infinity", std::isinf(value.f_infinity) &&
                               value.f_infinity > 0);
        report("d -Infinity", std::isinf(value.d_negative_infinity) &&
                                  value.d_negative_infinity < 0);
        report("NAN", std::isnan(value.f_upper_nan));
        report("nan(payload)", std::isnan(value.d_nan_payload));
    }
    // W-39: the uppercase escape decodes to the letter A.
    {
        UpperHexBytes value;
        const char expected[] = { 'A' };
        report("upper hex", is_bytes(value.data, expected, sizeof expected));
    }
    // R2 / B4: the escape table, on both literal kinds. Each row is the byte
    // sequence protoc 30.1 stores for the same source: an octal escape above
    // 0xFF is its low byte (CR-12), a surrogate pair is one code point and a
    // lone surrogate is its three bytes (CR-13), and a raw CR is content
    // (CR-29). A bytes default decodes to the same bytes as the string one.
    {
        lit::Escapes value;
        report("esc hex", is_bytes(value.s_hex, "A", 1) &&
                              is_bytes(value.s_hex_upper, "A", 1) &&
                              is_bytes(value.s_hex_one, "\t", 1));
        report("esc octal", is_bytes(value.s_octal, "A", 1));
        const char max_byte[] = { '\xff' };
        report("esc 777", is_bytes(value.s_octal_max, max_byte, 1));
        const char nul_middle[] = { 'a', '\0', 'b' };
        report("esc 400", is_bytes(value.s_octal_nul, nul_middle, 3));
        const char nul_only[] = { '\0' };
        report("esc 0", is_bytes(value.s_octal_one, nul_only, 1));
        const char u2[] = { '\xc3', '\xa9' };
        const char u3[] = { '\xe2', '\x82', '\xac' };
        const char astral[] = { '\xf0', '\x9f', '\x98', '\x80' };
        report("esc u", is_bytes(value.s_u1, "A", 1) &&
                            is_bytes(value.s_u2, u2, 2) &&
                            is_bytes(value.s_u3, u3, 3) &&
                            is_bytes(value.s_u4, astral, 4));
        const char lone_head[] = { '\xed', '\xa0', '\x80' };
        const char lone_tail[] = { '\xed', '\xb0', '\x80' };
        report("esc pair", is_bytes(value.s_pair, astral, 4));
        report("esc lone", is_bytes(value.s_lone_head, lone_head, 3) &&
                               is_bytes(value.s_lone_tail, lone_tail, 3));
        const char head_ascii[] = { '\xed', '\xa0', '\x80', 'A' };
        report("esc head ascii", is_bytes(value.s_head_ascii, head_ascii, 4));
        const char over_max[] = { '\\', 'U', '0', '0', '1', '1', '0', '0', '0', '0' };
        report("esc overmax", is_bytes(value.s_over_max, over_max, 10));
        const char raw_cr[] = { 'x', '\r', 'y' };
        report("esc raw cr", is_bytes(value.s_raw_cr, raw_cr, 3));
        report("esc bytes", is_bytes(value.b_octal_max, max_byte, 1) &&
                                is_bytes(value.b_octal_nul, nul_only, 1) &&
                                is_bytes(value.b_pair, astral, 4) &&
                                is_bytes(value.b_lone_head, lone_head, 3) &&
                                is_bytes(value.b_over_max, over_max, 10) &&
                                is_bytes(value.b_raw_cr, raw_cr, 3));
    }
    // R2 / B4: the lexeme table. `0x` is hex, a leading zero is octal, and a
    // zero followed by a fraction or exponent is decimal; the descriptor set
    // carries the float/double spellings protoc's descriptor validator accepts.
    {
        lit::Lexemes value;
        report("lex octal", value.i_hex == 16 && value.i_octal == 8 &&
                                value.i_octal_63 == 63 &&
                                value.i_octal_zero == 0 && value.i_zero == 0);
        report("lex float", value.f_octal == 8.0f && value.d_hex == 16.0 &&
                                value.d_fraction == 0.5 && value.d_exp == 0.0);
        report("lex uint64", value.u_hex_max ==
                                 std::numeric_limits<std::uint64_t>::max());
        // The non-default consumers: the octal enum value and reserved-range
        // endpoint are accepted, and the enum value decodes to 8.
        report("lex enum", static_cast<int>(lit::LEXEME_OCTAL) == 8);
    }
    {
        lit::DescriptorOctal value;
        report("desc octal", value.f08 == 8.0f && value.f09 == 9.0f &&
                                 value.d010 == 10.5 && value.i010 == 8);
    }
    // W-40: the accepted controls beside the refused plus spellings.
    {
        PlusControls value;
        report("plus int", value.i == 7);
        report("plus float", value.d == 1.0);
        report("plus inf", std::isinf(value.positive_infinity) &&
                               value.positive_infinity > 0);
        report("plus nan", std::isnan(value.quiet_nan));
    }
    {
        PlusDescriptorControls value;
        report("desc int", value.i == -7);
        report("desc float", value.d == -1.0);
    }
    // W-43: the warning fixtures and the carried names compile and keep the
    // descriptor text. Their values are not part of the runtime verdict.
    {
        JsonNamesDistinct value;
        (void)value;
        JsonCustomVsDefaultProto2 warning_value;
        (void)warning_value;
        JsonProto2Warning second_warning;
        (void)second_warning;
        EmptySyntaxDescriptor empty_syntax;
        (void)empty_syntax;
    }
    {
        AliasSame alias = ALIAS_SAME_FOO;
        report("alias same", alias == ALIAS_SAME_FOO);
    }
    {
        deep_package::DeepPackage101 deep;
        deep.value = 1;
        report("deep package", deep.value == 1);
    }
    // U-02: `package easypb;` is accepted and lands in the runtime namespace.
    // The type is generated and compiles beside <easypb.hpp>; the value travels
    // under the field number the schema gives it.
    {
        easypb::ValidityConfig value;
        value.value = 17;
        const easypb::ValidityConfig copy =
            easypb::decode<easypb::ValidityConfig>(easypb::encode(value));
        report("easypb package", copy.value == 17 && copy.has_value);
    }
    // V-02: the reserved-form rule is a file-scope rule, so the nested `_Foo`
    // keeps its spelling and is nameable from outside the generated struct.
    {
        app::Outer::_Foo nested;
        nested.x = 5;
        report("nested _Foo", nested.x == 5);
    }

    // The special floating-point defaults.
    {
        InfDefaults value;
        report("inf", std::isinf(value.positive_infinity) &&
                       value.positive_infinity > 0);
        report("-inf", std::isinf(value.negative_infinity) &&
                        value.negative_infinity < 0);
    }
    {
        NanDefaults value;
        report("nan", std::isnan(value.quiet_nan));
        report("nan float", std::isnan(value.quiet_nan_float));
    }

    if (failures != 0) {
        std::printf("%d default value(s) are wrong\n", failures);
        return 1;
    }
    std::printf("every default value carries the bytes the schema names\n");
    return 0;
}
