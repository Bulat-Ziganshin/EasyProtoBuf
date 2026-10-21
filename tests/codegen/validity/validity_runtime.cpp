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
#include <cstdio>
#include <cstring>
#include <string>

#include "defaults-backslash.pb.hpp"
#include "defaults-inf.pb.hpp"
#include "defaults-nan.pb.hpp"
#include "defaults-newline.pb.hpp"
#include "defaults-nul.pb.hpp"
#include "defaults-quote-byte.pb.hpp"
#include "defaults-quote.pb.hpp"
#include "defaults-trigraph.pb.hpp"

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
