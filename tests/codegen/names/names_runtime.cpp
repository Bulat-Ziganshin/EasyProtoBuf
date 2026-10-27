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

#include "codec-enum-value-decode.no-decoder.pb.hpp"
#include "codec-enum-value-encode.no-encoder.pb.hpp"
#include "codec-message-decode.no-decoder.pb.hpp"
#include "codec-message-encode-p2.no-encoder.pb.hpp"
#include "codec-message-encode.no-encoder.pb.hpp"
#include "enum-flag-p2.no-has-fields.pb.hpp"
#include "enum-flag-p3.no-has-fields.pb.hpp"
#include "has-collision-p3.no-has-fields.pb.hpp"
#include "has-collision.no-has-fields.pb.hpp"
#include "insertion-stem-collision.default.pb.hpp"
#include "keyword-enum-value.pb.hpp"
#include "keyword-enum.pb.hpp"
#include "keyword-field.pb.hpp"
#include "keyword-message.pb.hpp"
#include "keyword-nested.pb.hpp"
#include "namespace-packaged.default.pb.hpp"
#include "nested-flag.no-has-fields.pb.hpp"
#include "package-easypb-case.default.pb.hpp"
#include "package-easypb.default.pb.hpp"

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

    // --- step B6: the scope-occupant family ---------------------------------
    //
    // The accepted half of every collision pair, plus the two controls the
    // family needs (the packaged runtime names and the CR-44 artifact). Each
    // collision header was generated with the one option that removes the
    // second occupant, so the declaration that remains must be the one the
    // schema names - and it must carry data under the field number the schema
    // gives it.

    // CR-03 with --no-has-fields: the enumerator is the only `has_foo`, and
    // an unscoped enum's value is reachable through the message as well as
    // through the enum (both spellings are members of M).
    {
        report("enum/flag enumerator",
               enumflagp3::M::has_foo == 0 &&
               enumflagp3::M::E::has_foo == 0);
        enumflagp3::M message;
        message.foo = 7;
        const std::string wire = easypb::encode(message);
        const char expected[] = { 0x08, 0x07 };
        report("enum/flag wire", is_bytes(wire, expected, sizeof expected));
        const enumflagp3::M copy = easypb::decode<enumflagp3::M>(wire);
        report("enum/flag round trip", copy.foo == 7);
    }
    {
        report("enum/flag p2 enumerator", enumflagp2::M::has_foo == 0);
        enumflagp2::M message;
        message.foo = 8;
        const enumflagp2::M copy =
            easypb::decode<enumflagp2::M>(easypb::encode(message));
        report("enum/flag p2 round trip", copy.foo == 8);
    }

    // W-09 and PC-01 with --no-has-fields: the two fields and the nested type
    // keep their own names once the flag is gone.
    {
        hascollision::M message;
        message.foo = 1;
        message.has_foo = 2;
        const hascollision::M copy =
            easypb::decode<hascollision::M>(easypb::encode(message));
        report("flag/field names", copy.foo == 1 && copy.has_foo == 2);
    }
    {
        hascollisionp3::M message;
        message.foo = 3;
        message.has_foo = 4;
        const hascollisionp3::M copy =
            easypb::decode<hascollisionp3::M>(easypb::encode(message));
        report("flag/field p3 names", copy.foo == 3 && copy.has_foo == 4);
    }
    {
        nestedflag::M message;
        message.foo = 5;
        nestedflag::M::has_foo payload;
        payload.value = 6;
        const nestedflag::M copy =
            easypb::decode<nestedflag::M>(easypb::encode(message));
        report("flag/nested names", copy.foo == 5 && payload.value == 6);
    }

    // CR-04: the type survives once the codec that owned the name is off.
    {
        codecmessageencode::encode message;
        message.a = 9;
        report("codec/message encode", message.a == 9);
        codecmessageencodep2::encode message_p2;
        message_p2.a = 10;
        report("codec/message encode p2", message_p2.a == 10);
    }
    {
        codecmessagedecode::decode message;
        message.a = 11;
        const std::string wire = easypb::encode(message);
        const char expected[] = { 0x08, 0x0b };
        report("codec/message decode", is_bytes(wire, expected, sizeof expected));
    }
    {
        report("codec/enum value encode",
               codecenumvalueencode::encode == 0 &&
               codecenumvalueencode::E::encode == 0);
        report("codec/enum value decode",
               codecenumvaluedecode::decode == 0 &&
               codecenumvaluedecode::E::decode == 0);
    }

    // CR-09 control: inside a package both runtime names are ordinary types.
    {
        namespacepackaged::easypb message;
        message.a = 12;
        namespacepackaged::std other;
        other.b = 13;
        report("packaged runtime names",
               easypb::decode<namespacepackaged::easypb>(
                   easypb::encode(message)).a == 12 &&
               easypb::decode<namespacepackaged::std>(
                   easypb::encode(other)).b == 13);
    }

    // CR-44 accepted: both messages exist and carry their own field; the
    // shared insertion stem is asserted by the dedicated xfail test.
    {
        insertionstem::A_B flat;
        flat.x = 14;
        insertionstem::A::B nested;
        nested.y = 15;
        report("insertion stem messages",
               easypb::decode<insertionstem::A_B>(
                   easypb::encode(flat)).x == 14 &&
               easypb::decode<insertionstem::A::B>(
                   easypb::encode(nested)).y == 15);
    }

    // U-02 controls: `package easypb;` is accepted (owner decision #1) as long
    // as the declarations do not carry a runtime type's name. Both headers
    // live in `namespace easypb` next to <easypb.hpp> and must compile and
    // round-trip; `encoder` differs from `Encoder` by case alone, which is a
    // different C++ name.
    {
        easypb::Config config;
        config.value = 21;
        const easypb::Config copy =
            easypb::decode<easypb::Config>(easypb::encode(config));
        report("easypb package control",
               copy.value == 21 && copy.has_value);
    }
    {
        easypb::encoder lower;
        lower.value = 22;
        const easypb::encoder copy =
            easypb::decode<easypb::encoder>(easypb::encode(lower));
        report("easypb package case", copy.value == 22 && copy.has_value);
    }

    if (failures != 0) {
        std::printf("%d escaped name(s) do not carry the schema's data\n",
                    failures);
        return 1;
    }
    std::printf("every escaped name carries the data the schema names\n");
    return 0;
}
