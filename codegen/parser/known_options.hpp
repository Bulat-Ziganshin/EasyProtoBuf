#ifndef EASYPB_KNOWN_OPTIONS_HPP_INCLUDED
#define EASYPB_KNOWN_OPTIONS_HPP_INCLUDED

#include <cstddef>
#include <string>

namespace easypb_proto {

// D2-A: the validation-only known-option table for the pinned reference
// compiler (`protoc` 30.1). The table is closed: it names every field of the
// nine `*Options` messages in `descriptor.proto` except
// `uninterpreted_option` (custom options stay opaque) and the nine `features`
// fields (editions are out of scope). It is the only place that knows an
// option's owner, its value kind and the value-dependent legality rules; the
// parser asks it instead of scattering checks through the grammar.
//
// This is validation, not feature support: no option value is stored in the
// trimmed descriptor model beyond the four options that already have one
// (`default`, `json_name`, `packed`, `(easypb.cpp).type`).

// The declaration an option statement or a bracketed option list belongs to.
enum OptionOwnerKind
{
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

// The shape of the parsed constant, mirroring the parser's own token kinds
// without exposing them.
enum OptionConstantKind
{
    OPTION_CONSTANT_STRING,
    OPTION_CONSTANT_INTEGER,
    OPTION_CONSTANT_FLOAT,
    OPTION_CONSTANT_BOOL,
    OPTION_CONSTANT_IDENTIFIER,
    OPTION_CONSTANT_AGGREGATE,
};

// The declared kind of the field an option list belongs to. Only the
// field-only rules read it; every other owner passes a default instance.
// R09 (U-44): the struct carries no `repeated`/`named` flag. Those were
// written by the parser and never read by a rule - repeatedness is decided by
// the field provenance and a named type's kind by the shared semantic pass.
struct OptionFieldKind
{
    bool has_type;
    int type;        // FieldDescriptorProto::TYPE_*

    OptionFieldKind() : has_type(false), type(0) {}
};

struct KnownOption
{
    OptionOwnerKind owner;
    const char* name;
    OptionConstantKind value_kind;
    // Null-terminated enumerator names for an enum-valued option, else null.
    const char* const* enum_values;
    // True when the option is a repeated field, so several occurrences in one
    // scope are legal (`targets`, `declaration`, `edition_defaults`).
    bool repeated;
    // Non-empty when the reference compiler refuses this option name in source
    // text whatever its value (`map_entry`). The text is the refusal reason.
    const char* refuse_reason;
};

// Finds a known field of the owner's `*Options` message. Returns null for an
// unknown name or a name whose only owner is a different declaration.
const KnownOption* find_known_option(OptionOwnerKind owner, const std::string& name);

// The declaration scope an owner denotes, for diagnostics that name it
// ("file", "message", "extension range", ...).
const char* option_owner_name(OptionOwnerKind owner);

// Returns an empty string when the parenthesized custom `name` may be set at
// `owner`, and the scoped refusal otherwise. Custom options are otherwise
// opaque; `(easypb.cpp).type` is the one name with product semantics and is
// meaningful only on a field, so every other scope refuses it (R09, U-54).
std::string custom_option_scope_error(OptionOwnerKind owner,
                                      const std::string& name);

// The table as a sequence, for the family test that covers every row.
std::size_t known_option_count();
const KnownOption& known_option_at(std::size_t index);

// Returns an empty string when `value` is acceptable for `option` and the
// diagnostic text otherwise. The text names the rule, not the reference
// compiler's sentence. `field` carries the declared kind for the field-only
// rules.
std::string known_option_value_error(const KnownOption& option,
                                     OptionConstantKind value_kind,
                                     const std::string& text,
                                     const OptionFieldKind& field);

// The first accepted enumerator of an enum-valued option, for the family test.
// Returns null when the option is not enum-valued.
const char* known_option_first_enum_value(const KnownOption& option);

} // namespace easypb_proto

#endif
