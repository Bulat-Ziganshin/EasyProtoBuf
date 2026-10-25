// CR-08/P7 runtime probe: a default-initialized message whose enum member is
// defined in another file must read the protobuf default (the zero value here),
// not whatever the storage held. The placement-new over a 0xAB-filled buffer is
// the shape the CR-08 measurement used, and the header this file includes is
// the COMPATIBILITY-mode header the build generates from `p7-thin.pbs` (the
// thin set carries `b.proto` only, so `q.E` and `q.Ext` are external).
//
// The same member used to be declared without an initializer, so this probe
// read -1414812757; the thin header must now equal the complete closure's
// header byte for byte (W-68), which the lane script compares.

#include <cstddef>
#include <cstring>
#include <new>
#include <string>

#include "b.pb.hpp"

namespace {

// A zeroed buffer is the compiler's business, so the check spells the expected
// values as the schema does: `q.E.Z` is the first declared value, and both
// presence flags start clear.
bool zero_initialized()
{
    alignas(::p::M) unsigned char storage[sizeof(::p::M)];
    std::memset(storage, 0xAB, sizeof storage);
    ::p::M* value = new (storage) ::p::M;
    return value->e == ::q::E::Z && ! value->has_e && value->i == 0 &&
           ! value->has_i;
}

// The generated encoder reads the member unconditionally, so the value must be
// a declared enumerator. Encoding a zero-initialized message therefore starts
// with field 1's tag and the enum's zero value, and the whole read is defined.
bool encodes_the_zero_value()
{
    const std::string bytes = ::easypb::encode(::p::M());
    return bytes.size() >= 2 && bytes[0] == char(0x08) && bytes[1] == char(0x00);
}

} // namespace

int main()
{
    return zero_initialized() && encodes_the_zero_value() ? 0 : 1;
}
