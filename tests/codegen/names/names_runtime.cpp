// The names lane's runtime check: the escaped names carry the schema's data.
//
// A name that is escaped is only half of the fix. The other half is that the
// escaped member still holds the value the schema names, and that the value
// still travels under the field number the schema gives it: a member that
// was renamed but not wired to its field would compile cleanly and carry the
// wrong data.
//
// So this program writes every escaped member, checks the BYTES on the wire,
// and reads them back. The headers are the descriptor-path ones, because the
// descriptor path is the one every build profile has; the driver compares
// the source path's header with the descriptor path's byte for byte, so the
// names checked here are the names both paths produce.

#include <cstdio>
#include <cstring>
#include <string>

#include "keyword-enum-value.pb.hpp"
#include "keyword-enum.pb.hpp"
#include "keyword-field.pb.hpp"
#include "keyword-message.pb.hpp"
#include "keyword-nested.pb.hpp"

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

int main()
{
    // A field named after a keyword: the member is `class_`, and the value
    // still travels under the field number the schema gives it.
    {
        kwfield::M message;
        message.class_ = 42;
        message.template_ = 7;
        message.namespace_ = 9;
        const std::string wire = easypb::encode(message);
        const char expected[] = { 0x08, 0x2a, 0x10, 0x07, 0x18, 0x09 };
        report("keyword field wire", is_bytes(wire, expected, sizeof expected));

        const kwfield::M copy = easypb::decode<kwfield::M>(wire);
        report("keyword field round trip",
               copy.class_ == 42 && copy.template_ == 7 && copy.namespace_ == 9);
        // The presence flag is named after the ESCAPED field, and it is set
        // by the decoder exactly as the unescaped name would be.
        report("keyword field flags",
               copy.has_class_ && copy.has_template_ && copy.has_namespace_);
        report("keyword field flag names",
               !kwfield::M().has_class_ && !kwfield::M().has_template_ &&
               !kwfield::M().has_namespace_);
    }

    // A message named after a keyword: the type is `class_`, and a field of
    // that type carries the nested message's own value.
    {
        kwmessage::class_ payload;
        payload.value = 5;
        kwmessage::Holder holder;
        holder.payload = payload;
        const std::string wire = easypb::encode(holder);
        const kwmessage::Holder copy = easypb::decode<kwmessage::Holder>(wire);
        report("keyword message round trip", copy.payload.value == 5);
    }

    // An enum named after a keyword: the type is `class_`, and the default of
    // the field that uses it is the escaped reference to the enum's second
    // value.
    {
        kwenum::Holder defaults;
        report("keyword enum default",
               defaults.kind == kwenum::class_::second);

        kwenum::Holder holder;
        holder.kind = kwenum::class_::first;
        const std::string wire = easypb::encode(holder);
        const char expected[] = { 0x08, 0x00 };
        report("keyword enum wire", is_bytes(wire, expected, sizeof expected));

        const kwenum::Holder copy = easypb::decode<kwenum::Holder>(wire);
        report("keyword enum round trip", copy.kind == kwenum::class_::first);
    }

    // An enum VALUE named after a keyword: the enumerator is `class_`, and
    // the default of the field that uses it is the escaped reference to it.
    {
        kwenumvalue::Holder defaults;
        report("keyword enum value default",
               defaults.kind == kwenumvalue::E::class_);

        kwenumvalue::Holder holder;
        holder.kind = kwenumvalue::E::other;
        const std::string wire = easypb::encode(holder);
        const char expected[] = { 0x08, 0x01 };
        report("keyword enum value wire",
               is_bytes(wire, expected, sizeof expected));

        const kwenumvalue::Holder copy =
            easypb::decode<kwenumvalue::Holder>(wire);
        report("keyword enum value round trip",
               copy.kind == kwenumvalue::E::other);
    }

    // A nested message and a nested enum named after a keyword: both are
    // reachable through the enclosing struct, and the default of the field
    // that uses the nested enum is the escaped reference.
    {
        kwnested::Outer defaults;
        report("nested keyword default",
               defaults.kind == kwnested::Outer::template_::second);

        kwnested::Outer outer;
        outer.payload.value = 11;
        outer.kind = kwnested::Outer::template_::first;
        const std::string wire = easypb::encode(outer);
        const kwnested::Outer copy = easypb::decode<kwnested::Outer>(wire);
        report("nested keyword round trip",
               copy.payload.value == 11 &&
               copy.kind == kwnested::Outer::template_::first);
    }

    if (failures != 0) {
        std::printf("%d escaped name(s) do not carry the schema's data\n",
                    failures);
        return 1;
    }
    std::printf("every escaped name carries the data the schema names\n");
    return 0;
}
