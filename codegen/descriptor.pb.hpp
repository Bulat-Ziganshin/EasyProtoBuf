// This file will be auto-generated when I grow up (with option "-s str_view").
// Source: https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto
#ifndef EASYPB_DESCRIPTOR_PB_HPP_INCLUDED
#define EASYPB_DESCRIPTOR_PB_HPP_INCLUDED

#include <cstdint>
#include <string>
#include <vector>
#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#include <string_view>
#endif

#ifdef __cpp_lib_string_view
using str_view = std::string_view;  // Might be a little faster with C++17
#else
using str_view = std::string;
#endif

#include <easypb.hpp>


struct OneofDescriptorProto
{
    str_view name;

    bool has_name = false;
};


// Single enum value
struct EnumValueDescriptorProto
{
    str_view name;
    int32_t number = 0;

    bool has_name = false;
    bool has_number = false;
};


// Enum options. Only `allow_alias` is modeled: with it set, two values of one
// enum may share a number, and without it that shape is a defect the linker
// refuses (D-28). It is the second field of EnumOptions in descriptor.proto.
struct EnumOptions
{
    bool allow_alias = false;

    bool has_allow_alias = false;
};


// One `reserved START to END;` declaration of an enum (wire fields 1 and 2 of
// `EnumDescriptorProto.EnumReservedRange`, field 4 of `EnumDescriptorProto`).
// Both ends are INCLUSIVE here, unlike the message twin: descriptor.proto says
// so explicitly, because the inclusive form can represent the whole int32
// domain (descriptor.proto:356-365). Both frontends retain the range: the
// source parser stores what it validated (V-15) and the descriptor door
// decodes it, so the reference-free sweep asks whether a value uses a reserved
// number and whether two ranges overlap (CR-06).
struct EnumDescriptorProto_EnumReservedRange
{
    int32_t start = 0;  // field 1, inclusive
    int32_t end = 0;    // field 2, inclusive

    bool has_start = false;
    bool has_end = false;
};


// Enum
struct EnumDescriptorProto
{
    str_view name;
    std::vector<EnumValueDescriptorProto> value;
    EnumOptions options;
    // Wire fields 4 and 5: the enum's `reserved` ranges and names. The
    // reference-free reserved sweep answers protoc's two questions about them
    // (`BuildEnum`, descriptor.cc:7060-7125) whether or not the enum is
    // referenced (CR-06).
    std::vector<EnumDescriptorProto_EnumReservedRange> reserved_range;
    std::vector<str_view> reserved_name;

    bool has_name = false;
    bool has_options = false;
};


// EasyProtoBuf-specific field options. The extension number is mirrored by
// codegen/easypb/options.proto so .proto parsing and .pbs decoding normalize
// into the same trimmed descriptor representation.
enum { EASYPB_CPP_FIELD_OPTIONS_NUMBER = 52777 };

struct CppFieldOptions
{
    str_view type;

    bool has_type = false;
};


struct FieldOptions
{
    // Wire field 6: which JavaScript type a 64-bit integral field is exposed
    // as. `descriptor.proto:699-709` declares the enumerators below; a value
    // outside them is not modeled, exactly as protoc routes an unknown
    // enumerator of a closed proto2 enum to its unknown fields, which leaves
    // `has_jstype` false.
    enum {
        JS_NORMAL = 0,
        JS_STRING = 1,
        JS_NUMBER = 2,
    };

    bool packed = false;
    // Wire fields 5, 6 and 15: `lazy`, `jstype` and `unverified_lazy`. protoc
    // refuses `[lazy = true]`/`[unverified_lazy = true]` on anything but a
    // submessage field (`ValidateOptions`, descriptor.cc:7968-7974) and a
    // non-normal `jstype` outside the 64-bit integral kinds (`ValidateJSType`,
    // descriptor.cc:8581-8617). Both rules are asked over the decoded option
    // (schema_semantics.cpp), so the descriptor door refuses what the source
    // door refuses.
    int32_t jstype = JS_NORMAL;
    bool lazy = false;
    bool unverified_lazy = false;
    CppFieldOptions cpp;

    bool has_packed = false;
    bool has_jstype = false;
    bool has_lazy = false;
    bool has_unverified_lazy = false;
    bool has_cpp = false;
};


// Field
struct FieldDescriptorProto
{
    enum {
        TYPE_DOUBLE = 1,
        TYPE_FLOAT = 2,
        TYPE_INT64 = 3,
        TYPE_UINT64 = 4,
        TYPE_INT32 = 5,
        TYPE_FIXED64 = 6,
        TYPE_FIXED32 = 7,
        TYPE_BOOL = 8,
        TYPE_STRING = 9,
        TYPE_GROUP = 10,
        TYPE_MESSAGE = 11,
        TYPE_BYTES = 12,
        TYPE_UINT32 = 13,
        TYPE_ENUM = 14,
        TYPE_SFIXED32 = 15,
        TYPE_SFIXED64 = 16,
        TYPE_SINT32 = 17,
        TYPE_SINT64 = 18,
    };

    enum {
      LABEL_OPTIONAL = 1,
      LABEL_REPEATED = 3,
      LABEL_REQUIRED = 2,
    };

    str_view name;
    int32_t number = 0;
    int32_t label = 0;
    int32_t type = 0;
    str_view type_name;
    // Wire field 2: the extended message of an extension member. Modelled
    // since `V-09`, because the descriptor door now validates the members it
    // used to skip; an ordinary field never carries it, and the extension
    // sweep is the only reader.
    str_view extendee;
    str_view default_value;
    FieldOptions options;
    int32_t oneof_index = 0;
    str_view json_name;
    // Wire field 17: this field was written with an explicit `optional` label
    // in a proto3 file, so it has explicit presence. protoc models that as
    // this flag plus a synthetic one-field oneof the field claims
    // (`parser.cc:745-780`); the source parser synthesizes the same pair
    // (D1-A), so both frontends describe one logical file identically.
    bool proto3_optional = false;

    bool has_name = false;
    bool has_number = false;
    bool has_label = false;
    bool has_type = false;
    bool has_type_name = false;
    bool has_extendee = false;
    bool has_default_value = false;
    bool has_options = false;
    bool has_oneof_index = false;
    bool has_json_name = false;
    bool has_proto3_optional = false;
};


struct MessageOptions
{
    // Wire field 1: the message uses the MessageSet wire format. protoc
    // refuses a MessageSet that declares ordinary fields ("MessageSets cannot
    // have fields, only extensions.", descriptor.cc:6372-6380, :7994-8001)
    // and the reference-free sweep asks the same question (CR-23); the option
    // also lifts the extension-range upper bound, because extension numbers of
    // a MessageSet are plain int32s (`ValidateExtensionRangeOptions`,
    // descriptor.cc:8385-8406). Wire field 7 is the synthetic map-entry flag.
    bool message_set_wire_format = false;
    bool map_entry = false;

    bool has_message_set_wire_format = false;
    bool has_map_entry = false;
};


// One `reserved START to END;` declaration of a message (wire fields 1 and 2
// of `DescriptorProto.ReservedRange`, field 9 of `DescriptorProto`). `start`
// is INCLUSIVE and `end` EXCLUSIVE, exactly as descriptor.proto defines the
// pair (descriptor.proto:168-174). The ranges are retained so the
// reference-free reserved sweep can ask whether a declared field number falls
// inside one, whether two ranges overlap and whether an extension range
// overlaps one (CR-06).
struct DescriptorProto_ReservedRange
{
    int32_t start = 0;  // field 1, inclusive
    int32_t end = 0;    // field 2, exclusive

    bool has_start = false;
    bool has_end = false;
};


// One `declaration` of an extension range (`ExtensionRangeOptions.Declaration`,
// descriptor.proto:185-208). The declaration names the extension an extension
// number is reserved for; protoc checks that the number lies inside its range,
// that no number is declared twice, and that `full_name` and `type` are
// either both present or the declaration is `reserved`
// (`ValidateExtensionDeclaration`, descriptor.cc:8311-8375).
struct ExtensionRangeOptions_Declaration
{
    int32_t number = 0;     // field 1
    str_view full_name;     // field 2
    str_view type;          // field 3
    bool reserved = false;  // field 5
    bool repeated = false;  // field 6

    bool has_number = false;
    bool has_full_name = false;
    bool has_type = false;
    bool has_reserved = false;
    bool has_repeated = false;
};


// The options of one extension range (wire field 3 of
// `DescriptorProto.ExtensionRange`). `verification` is a closed proto2 enum
// (`descriptor.proto:219-229`): an enumerator outside the two below is routed
// to the unknown fields, exactly as protoc does, so `has_verification` only
// records a modeled value. `features` (field 50) and `uninterpreted_option`
// (field 999) are not modeled - no rule of this project reads them.
struct ExtensionRangeOptions
{
    enum {
        DECLARATION = 0,
        UNVERIFIED = 1,
    };

    std::vector<ExtensionRangeOptions_Declaration> declaration;  // field 2
    int32_t verification = UNVERIFIED;  // field 3, the descriptor.proto default
    bool has_verification = false;
};


// One `extensions START to END [declaration = {...}, verification = ...];`
// declaration. `start` is inclusive and `end` exclusive, exactly as
// descriptor.proto defines the pair
// (`DescriptorProto.ExtensionRange.start`/`.end`). The plan-5 follow-up's
// session S5 (`W-45`) models the pair instead of counting it: protoc refuses a
// proto3 extension range and a range that includes a declared field number
// (`descriptor.cc:6426-6430`, `:7898`), and neither question can be answered
// from a count. Wire field 3 carries the range's options (U-22).
struct DescriptorProto_ExtensionRange
{
    int32_t start = 0;  // field 1, inclusive
    int32_t end = 0;    // field 2, exclusive
    // Field 3: `ExtensionRangeOptions`, decoded since U-22. protoc refuses
    // `verification = UNVERIFIED` beside a declaration and checks the
    // declarations themselves (`ValidateExtensionRangeOptions`,
    // descriptor.cc:8377-8425); both need the options modeled.
    ExtensionRangeOptions options;

    bool has_start = false;
    bool has_end = false;
    bool has_options = false;
};


// Message
struct DescriptorProto
{
    str_view name;
    std::vector<FieldDescriptorProto> field;
    std::vector<DescriptorProto> nested_type;
    std::vector<EnumDescriptorProto> enum_type;
    std::vector<OneofDescriptorProto> oneof_decl;
    MessageOptions options;
    // Wire field 5: the extension ranges themselves, decoded since `W-45`
    // (the proto3 and field-overlap rules need their numbers).
    std::vector<DescriptorProto_ExtensionRange> extension_range;
    // Wire field 6 (`extension`): the extension members declared inside this
    // message. Decoded since `V-09`: the descriptor door used to count the
    // occurrences and skip the bytes, so extension tags, extendee ranges,
    // duplicates and MessageSet constraints were never validated. The members
    // are still not emitted (E6), but the semantic pass now answers protoc's
    // rules for them.
    std::vector<FieldDescriptorProto> extension;  // field 6
    // Wire fields 9 and 10: the message's `reserved` ranges and names. The
    // reference-free reserved sweep answers protoc's five message questions
    // about them (`BuildMessage`, descriptor.cc:6396-6478) whether or not the
    // message is referenced (CR-06).
    std::vector<DescriptorProto_ReservedRange> reserved_range;
    std::vector<str_view> reserved_name;

    bool has_name = false;
    bool has_options = false;
};


// Single .proto method. The NAME is retained so the lexical name sweep can
// check it (ledger `D-57`), the input/output type names are retained so the
// linker can resolve them (`W-16`): protoc's `CrossLinkMethod` refuses a name
// that resolves nowhere and one that resolves to something other than a
// message (`descriptor.cc:7714-7757`), and the source frontend used to discard
// the whole method table, so neither shape could ever be refused there.
//
// CR-48: the two streaming flags are decoded too. They carry no generated-code
// effect, but `--print-descriptor` is a diagnostics mode, and a service it
// prints without its streaming markers would misdescribe the input.
struct MethodDescriptorProto
{
    str_view name;
    str_view input_type;   // field 2
    str_view output_type;  // field 3
    bool client_streaming = false;  // field 5
    bool server_streaming = false;  // field 6

    bool has_name = false;
    bool has_input_type = false;
    bool has_output_type = false;
    bool has_client_streaming = false;
    bool has_server_streaming = false;
};


// Single .proto service. The name is retained so it participates in
// package-scope collision validation; the method names are retained so the
// lexical name sweep can cover them, which is what closes `D-57`.
struct ServiceDescriptorProto
{
    str_view name;
    std::vector<MethodDescriptorProto> method;

    bool has_name = false;
};


// Single .proto file
struct FileDescriptorProto
{
    str_view name;
    str_view package;
    std::vector<DescriptorProto> message_type;
    std::vector<EnumDescriptorProto> enum_type;
    std::vector<ServiceDescriptorProto> service;
    str_view syntax;
    // Field 14: the edition a file declares (`edition = "2023";`). protoc 30.1
    // writes it for editions files only, beside `syntax = "editions"`; the
    // classifier consults it FIRST, mirroring `BuildFileImpl`
    // (descriptor.cc:5952-5971), so a crafted set that carries the field with
    // no syntax string is classified like the reference and not downgraded to
    // proto2 (ledger `D-52`).
    int32_t edition = 0;
    std::vector<str_view> dependency;
    std::vector<int32_t> public_dependency;
    std::vector<int32_t> weak_dependency;
    // Wire field 7 (`extension`): file-scope extension members. Decoded since
    // `V-09` for the same reason as `DescriptorProto.extension`; the rows a
    // descriptor can carry but the source parser discards are still not
    // emitted (E6).
    std::vector<FieldDescriptorProto> extension;  // field 7

    bool has_name = false;
    bool has_package = false;
    bool has_syntax = false;
    bool has_edition = false;
};


// Multiple .proto files
struct FileDescriptorSet
{
    std::vector<FileDescriptorProto> file;
};


// The descriptor decoder's strict cursor.
//
// `decode` receives the runtime cursor by value. Every descriptor message
// reads through this wrapper instead, so a scalar field is only accepted with
// the wire type `descriptor.proto` declares for it. The runtime getters
// deliberately convert FIXED32/FIXED64 into integral fields
// (`Reader::parse_integer_value`, the `wire.type.conversions` contract); a
// descriptor set is a schema DEFINITION, so a converted value would silently
// rewrite the schema instead of describing it - a `ReservedRange.start`
// written as FIXED32 used to disappear from the model (U-58). Only this file
// is strict; a user message keeps the documented tolerance.
//
// A length-delimited field is checked by the runtime itself (the byte-array
// and message readers accept nothing else), so only the integral families need
// a wire-type statement here. `decode` for a nested message is reached with
// this type, which is what keeps the strictness on every level.
struct DescriptorDecoder : easypb::Decoder
{
    // Deliberately converting: `easypb::decode<T>(bytes)` (and a direct
    // `decode(Decoder(...), x)` call) passes the runtime cursor, and the
    // one-step conversion is what turns every descriptor decode into a strict
    // one.
    DescriptorDecoder(const easypb::Decoder& cursor)
        : easypb::Decoder(cursor)
    {
    }

    explicit DescriptorDecoder(easypb::string_view view)
        : easypb::Decoder(view)
    {
    }

    void get_int32(int32_t* field, bool* has_field = 0)
    {
        require_wire_type(easypb::WIRETYPE_VARINT);
        easypb::Decoder::get_int32(field, has_field);
    }

    void get_bool(bool* field, bool* has_field = 0)
    {
        require_wire_type(easypb::WIRETYPE_VARINT);
        easypb::Decoder::get_bool(field, has_field);
    }

    void get_enum(int32_t* field, bool* has_field = 0)
    {
        require_wire_type(easypb::WIRETYPE_VARINT);
        easypb::Decoder::get_enum(field, has_field);
    }

    void get_repeated_int32(std::vector<int32_t>* field)
    {
        // A repeated numeric field may be written packed or one value at a
        // time: both are canonical for descriptor.proto, while a fixed-width
        // encoding of the same field is not.
        require_wire_type(easypb::WIRETYPE_VARINT,
                          easypb::WIRETYPE_LENGTH_DELIMITED);
        easypb::Decoder::get_repeated_int32(field);
    }
};


inline void decode(DescriptorDecoder pb, OneofDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, EnumValueDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_int32(&x.number, &x.has_number); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, EnumOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_bool(&x.allow_alias, &x.has_allow_alias); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, EnumDescriptorProto_EnumReservedRange &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32(&x.start, &x.has_start); break;
            case 2: pb.get_int32(&x.end, &x.has_end); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, EnumDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.value); break;
            case 3: pb.get_message(&x.options, &x.has_options); break;
            case 4: pb.get_repeated_message(&x.reserved_range); break;
            case 5: pb.get_repeated_string(&x.reserved_name); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, CppFieldOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.type, &x.has_type); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, ExtensionRangeOptions_Declaration &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32 (&x.number,    &x.has_number); break;
            case 2: pb.get_string(&x.full_name, &x.has_full_name); break;
            case 3: pb.get_string(&x.type,      &x.has_type); break;
            case 5: pb.get_bool  (&x.reserved,  &x.has_reserved); break;
            case 6: pb.get_bool  (&x.repeated,  &x.has_repeated); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, ExtensionRangeOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_repeated_message(&x.declaration); break;
            // Field 3 is a closed proto2 enum. An enumerator outside the two
            // `descriptor.proto` declares is an unknown field, exactly as
            // protoc's own decoder routes it, so the presence bit stays false
            // and the value keeps the descriptor default.
            case 3:
            {
                int32_t verification = 0;
                pb.get_enum(&verification);
                if (verification == ExtensionRangeOptions::DECLARATION ||
                    verification == ExtensionRangeOptions::UNVERIFIED) {
                    x.verification = verification;
                    x.has_verification = true;
                }
                break;
            }
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, FieldOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_bool(&x.packed, &x.has_packed); break;
            case 5: pb.get_bool(&x.lazy, &x.has_lazy); break;
            case 15: pb.get_bool(&x.unverified_lazy, &x.has_unverified_lazy); break;
            // Field 6 is a closed proto2 enum: an unknown enumerator is an
            // unknown field, as in protoc's decoder.
            case 6:
            {
                int32_t jstype = FieldOptions::JS_NORMAL;
                pb.get_enum(&jstype);
                if (jstype == FieldOptions::JS_NORMAL ||
                    jstype == FieldOptions::JS_STRING ||
                    jstype == FieldOptions::JS_NUMBER) {
                    x.jstype = jstype;
                    x.has_jstype = true;
                }
                break;
            }
            case EASYPB_CPP_FIELD_OPTIONS_NUMBER:
                pb.get_message(&x.cpp, &x.has_cpp); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, FieldDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name,          &x.has_name); break;
            case 2: pb.get_string(&x.extendee,      &x.has_extendee); break;
            case 3: pb.get_int32 (&x.number,        &x.has_number); break;
            case 4: pb.get_enum  (&x.label,         &x.has_label); break;
            case 5: pb.get_enum  (&x.type,          &x.has_type); break;
            case 6: pb.get_string(&x.type_name,     &x.has_type_name); break;
            case 7: pb.get_string(&x.default_value, &x.has_default_value); break;
            case 8: pb.get_message(&x.options,      &x.has_options); break;
            case 9: pb.get_int32 (&x.oneof_index,   &x.has_oneof_index); break;
            case 10: pb.get_string(&x.json_name,    &x.has_json_name); break;
            case 17: pb.get_bool  (&x.proto3_optional,
                                   &x.has_proto3_optional); break;
            default: pb.skip_field();
        }
    }

    // CR-46: `name` is optional in descriptor.proto. An absent field name is a
    // semantic defect the lexical name sweep reports ("field in ... has no
    // name"), not a decode failure that aborts the whole set.
}


inline void decode(DescriptorDecoder pb, MessageOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_bool(&x.message_set_wire_format,
                                &x.has_message_set_wire_format); break;
            case 7: pb.get_bool(&x.map_entry, &x.has_map_entry); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, DescriptorProto_ReservedRange &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32(&x.start, &x.has_start); break;
            case 2: pb.get_int32(&x.end, &x.has_end); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, DescriptorProto_ExtensionRange &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32(&x.start, &x.has_start); break;
            case 2: pb.get_int32(&x.end, &x.has_end); break;
            case 3: pb.get_message(&x.options, &x.has_options); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, DescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.field); break;
            case 3: pb.get_repeated_message(&x.nested_type); break;
            case 4: pb.get_repeated_message(&x.enum_type); break;
            case 5: pb.get_repeated_message(&x.extension_range); break;
            // Field 6 is a message field: the members are decoded by the
            // schema model since `V-09` and validated by the extension sweep.
            case 6: pb.get_repeated_message(&x.extension); break;
            case 8: pb.get_repeated_message(&x.oneof_decl); break;
            case 7: pb.get_message(&x.options, &x.has_options); break;
            case 9: pb.get_repeated_message(&x.reserved_range); break;
            case 10: pb.get_repeated_string(&x.reserved_name); break;
            default: pb.skip_field();
        }
    }

    // descriptor.proto marks `name` optional, so the decoder must not invent a
    // "required field" refusal here (CR-46): an absent name is a semantic
    // defect, and `validate_symbol_names` reports it per declaration with the
    // declaration's own descriptor path ("message in ... has no name"). The
    // three throws this replaces aborted the whole set before any sweep could
    // name the offending declaration.
}


inline void decode(DescriptorDecoder pb, MethodDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_string(&x.input_type, &x.has_input_type); break;
            case 3: pb.get_string(&x.output_type, &x.has_output_type); break;
            case 5: pb.get_bool(&x.client_streaming, &x.has_client_streaming); break;
            case 6: pb.get_bool(&x.server_streaming, &x.has_server_streaming); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, ServiceDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.method); break;
            default: pb.skip_field();
        }
    }

    // CR-46: `ServiceDescriptorProto.name` is optional in descriptor.proto;
    // `validate_symbol_names` owns the refusal and names the declaration.
}


inline void decode(DescriptorDecoder pb, FileDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_string(&x.package, &x.has_package); break;
            case 3: pb.get_repeated_string(&x.dependency); break;
            case 4: pb.get_repeated_message(&x.message_type); break;
            case 5: pb.get_repeated_message(&x.enum_type); break;
            case 6: pb.get_repeated_message(&x.service); break;
            // Field 7: file-scope extension members (since `V-09`).
            case 7: pb.get_repeated_message(&x.extension); break;
            case 10: pb.get_repeated_int32(&x.public_dependency); break;
            case 11: pb.get_repeated_int32(&x.weak_dependency); break;
            case 12: pb.get_string(&x.syntax, &x.has_syntax); break;
            case 14: pb.get_enum  (&x.edition, &x.has_edition); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(DescriptorDecoder pb, FileDescriptorSet &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_repeated_message(&x.file); break;
            default: pb.skip_field();
        }
    }
}

#endif
