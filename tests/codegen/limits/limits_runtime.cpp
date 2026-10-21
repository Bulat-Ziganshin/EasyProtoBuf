// The limits lane's runtime check: the accepted nesting carries the schema's
// data.
//
// A depth limit is only half of the fix if the schema it accepts is still
// generated correctly: the 31 levels the reference compiler accepts have to
// produce a header that compiles and that carries the field the schema names,
// all the way down to the innermost message.
//
// So this program builds the innermost message of the 31-level chain, writes
// its field, compares the BYTES on the wire, and reads them back. The header
// is the descriptor-path one, because the descriptor path is the one every
// build profile has; the driver compares the source path's header with the
// descriptor path's byte for byte, so the types checked here are the types
// both paths produce.

#include <cstdio>
#include <cstring>
#include <string>

#include "nesting-31.pb.hpp"

namespace {

int failures = 0;

void report(const char* what, bool ok)
{
    std::printf("%-24s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

bool is_bytes(const std::string& value, const char* expected, std::size_t size)
{
    return value.size() == size &&
           std::memcmp(value.data(), expected, size) == 0;
}

} // namespace

// The innermost message of the 31-level chain, spelled out: a file-level
// message is M0, and every level below it adds one name.
typedef M0::M1::M2::M3::M4::M5::M6::M7::M8::M9::M10::M11::M12::M13::M14::M15::M16::M17::M18::M19::M20::M21::M22::M23::M24::M25::M26::M27::M28::M29::M30 deepest_message;

// A message in the middle of the chain: the type exists at that level too,
// which is what the nesting means.
typedef M0::M1::M2::M3::M4::M5::M6::M7::M8::M9::M10::M11::M12::M13::M14::M15::M16::M17::M18::M19::M20::M21::M22::M23::M24 middle_message;

int main()
{
    // The deepest message carries its own field, and the value travels under
    // the field number the schema gives it.
    deepest_message message;
    message.x = 42;
    const std::string wire = easypb::encode(message);
    const char expected[] = { 0x08, 0x2a };
    report("deepest wire", is_bytes(wire, expected, sizeof expected));

    const deepest_message copy = easypb::decode<deepest_message>(wire);
    report("deepest round trip", copy.x == 42);

    // A message in the middle of the chain is a type of its own and carries
    // its field the same way.
    middle_message middle;
    middle.x = 7;
    const std::string middle_wire = easypb::encode(middle);
    const char middle_expected[] = { 0x08, 0x07 };
    report("middle wire", is_bytes(middle_wire, middle_expected,
                                   sizeof middle_expected));

    // The outermost message is one more level of the same.
    M0 outer;
    outer.x = 1;
    const std::string outer_wire = easypb::encode(outer);
    const char outer_expected[] = { 0x08, 0x01 };
    report("outer wire", is_bytes(outer_wire, outer_expected,
                                  sizeof outer_expected));

    if (failures == 0) {
        std::printf("every accepted nesting carries the schema's data\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", failures);
    return 1;
}
