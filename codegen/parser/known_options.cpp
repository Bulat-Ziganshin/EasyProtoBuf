#include "known_options.hpp"

#include "descriptor.pb.hpp"

namespace easypb_proto {
namespace {

// `descriptor.proto` 30.1 enumerator lists, in declaration order. The first
// entry is the one the family test uses as an accepted value.
const char* const OPTIMIZE_FOR_VALUES[] = { "SPEED", "CODE_SIZE", "LITE_RUNTIME", 0 };
const char* const VERIFICATION_STATE_VALUES[] = { "DECLARATION", "UNVERIFIED", 0 };
const char* const CTYPE_VALUES[] = { "STRING", "CORD", "STRING_PIECE", 0 };
const char* const JSTYPE_VALUES[] = { "JS_NORMAL", "JS_STRING", "JS_NUMBER", 0 };
const char* const OPTION_RETENTION_VALUES[] = {
    "RETENTION_UNKNOWN", "RETENTION_RUNTIME", "RETENTION_SOURCE", 0 };
const char* const OPTION_TARGET_TYPE_VALUES[] = {
    "TARGET_TYPE_UNKNOWN", "TARGET_TYPE_FILE", "TARGET_TYPE_EXTENSION_RANGE",
    "TARGET_TYPE_MESSAGE", "TARGET_TYPE_FIELD", "TARGET_TYPE_ONEOF",
    "TARGET_TYPE_ENUM", "TARGET_TYPE_ENUM_ENTRY", "TARGET_TYPE_SERVICE",
    "TARGET_TYPE_METHOD", 0 };
const char* const IDEMPOTENCY_LEVEL_VALUES[] = {
    "IDEMPOTENCY_UNKNOWN", "NO_SIDE_EFFECTS", "IDEMPOTENT", 0 };

// One row per field of the nine `*Options` messages, in `descriptor.proto`
// order, excluding `uninterpreted_option` and `features`. 47 rows.
const KnownOption KNOWN_OPTIONS[] = {
    // ExtensionRangeOptions
    { OPTION_OWNER_EXTENSION_RANGE, "declaration", OPTION_CONSTANT_AGGREGATE, 0, true, 0 },
    { OPTION_OWNER_EXTENSION_RANGE, "verification", OPTION_CONSTANT_IDENTIFIER,
      VERIFICATION_STATE_VALUES, false, 0 },

    // FileOptions
    { OPTION_OWNER_FILE, "java_package", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "java_outer_classname", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "java_multiple_files", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "java_generate_equals_and_hash", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "java_string_check_utf8", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "optimize_for", OPTION_CONSTANT_IDENTIFIER,
      OPTIMIZE_FOR_VALUES, false, 0 },
    { OPTION_OWNER_FILE, "go_package", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "cc_generic_services", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "java_generic_services", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "py_generic_services", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "cc_enable_arenas", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FILE, "objc_class_prefix", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "csharp_namespace", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "swift_prefix", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "php_class_prefix", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "php_namespace", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "php_metadata_namespace", OPTION_CONSTANT_STRING, 0, false, 0 },
    { OPTION_OWNER_FILE, "ruby_package", OPTION_CONSTANT_STRING, 0, false, 0 },

    // MessageOptions
    { OPTION_OWNER_MESSAGE, "message_set_wire_format", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_MESSAGE, "no_standard_descriptor_accessor", OPTION_CONSTANT_BOOL,
      0, false, 0 },
    { OPTION_OWNER_MESSAGE, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_MESSAGE, "map_entry", OPTION_CONSTANT_BOOL, 0, false,
      "map_entry should not be set explicitly. Use map<KeyType, ValueType> "
      "instead" },
    // U-55 / SD-1 (2026-10-27): `descriptor.proto` marks this field
    // `[deprecated = true]` and nothing reads it. The row stays so a migration
    // leftover is accepted; the option is a documented no-op with an
    // executable guard (`codegen.options.legacy_json_xfail`, OPTIONS.md).
    { OPTION_OWNER_MESSAGE, "deprecated_legacy_json_field_conflicts",
      OPTION_CONSTANT_BOOL, 0, false, 0 },

    // FieldOptions
    { OPTION_OWNER_FIELD, "ctype", OPTION_CONSTANT_IDENTIFIER, CTYPE_VALUES, false, 0 },
    { OPTION_OWNER_FIELD, "packed", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "jstype", OPTION_CONSTANT_IDENTIFIER, JSTYPE_VALUES, false, 0 },
    { OPTION_OWNER_FIELD, "lazy", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "unverified_lazy", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "weak", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "debug_redact", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_FIELD, "retention", OPTION_CONSTANT_IDENTIFIER,
      OPTION_RETENTION_VALUES, false, 0 },
    { OPTION_OWNER_FIELD, "targets", OPTION_CONSTANT_IDENTIFIER,
      OPTION_TARGET_TYPE_VALUES, true, 0 },
    { OPTION_OWNER_FIELD, "edition_defaults", OPTION_CONSTANT_AGGREGATE, 0, true, 0 },
    { OPTION_OWNER_FIELD, "feature_support", OPTION_CONSTANT_AGGREGATE, 0, false, 0 },

    // OneofOptions has no known fields: every unparenthesized option inside a
    // oneof is refused, as `protoc` refuses it (only `uninterpreted_option`
    // and `features` live there).

    // EnumOptions
    { OPTION_OWNER_ENUM, "allow_alias", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_ENUM, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    // The enum owner of the same U-55 / SD-1 no-op row.
    { OPTION_OWNER_ENUM, "deprecated_legacy_json_field_conflicts",
      OPTION_CONSTANT_BOOL, 0, false, 0 },

    // EnumValueOptions
    { OPTION_OWNER_ENUM_VALUE, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_ENUM_VALUE, "debug_redact", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_ENUM_VALUE, "feature_support", OPTION_CONSTANT_AGGREGATE, 0, false, 0 },

    // ServiceOptions
    { OPTION_OWNER_SERVICE, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },

    // MethodOptions
    { OPTION_OWNER_METHOD, "deprecated", OPTION_CONSTANT_BOOL, 0, false, 0 },
    { OPTION_OWNER_METHOD, "idempotency_level", OPTION_CONSTANT_IDENTIFIER,
      IDEMPOTENCY_LEVEL_VALUES, false, 0 },
};

const std::size_t KNOWN_OPTION_COUNT =
    sizeof(KNOWN_OPTIONS) / sizeof(KNOWN_OPTIONS[0]);

// The 64-bit integer kinds `jstype` is legal on (`ValidateJSType`,
// descriptor.cc:8590-8615).
bool is_64_bit_integer(int type)
{
    switch (type) {
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_FIXED64:
        case FieldDescriptorProto::TYPE_SFIXED64:
            return true;
        default:
            return false;
    }
}

bool names_contain(const char* const* names, const std::string& value)
{
    if (names == 0) return false;
    for (std::size_t i = 0; names[i] != 0; ++i) {
        if (value == names[i]) return true;
    }
    return false;
}

} // namespace


const KnownOption* find_known_option(OptionOwnerKind owner,
                                     const std::string& name)
{
    for (std::size_t i = 0; i < KNOWN_OPTION_COUNT; ++i) {
        if (KNOWN_OPTIONS[i].owner == owner && name == KNOWN_OPTIONS[i].name) {
            return &KNOWN_OPTIONS[i];
        }
    }
    return 0;
}


std::size_t known_option_count()
{
    return KNOWN_OPTION_COUNT;
}


const KnownOption& known_option_at(std::size_t index)
{
    return KNOWN_OPTIONS[index];
}


const char* known_option_first_enum_value(const KnownOption& option)
{
    if (option.enum_values == 0 || option.enum_values[0] == 0) return 0;
    return option.enum_values[0];
}


std::string known_option_value_error(const KnownOption& option,
                                     OptionConstantKind value_kind,
                                     const std::string& text,
                                     const OptionFieldKind& field)
{
    if (option.refuse_reason != 0) return std::string(option.refuse_reason);

    switch (option.value_kind) {
        case OPTION_CONSTANT_STRING:
            if (value_kind != OPTION_CONSTANT_STRING) {
                return "Value must be quoted string for string option \"" +
                       std::string(option.name) + "\"";
            }
            return std::string();

        case OPTION_CONSTANT_BOOL:
            if (value_kind != OPTION_CONSTANT_BOOL) {
                return "Value must be identifier for boolean option \"" +
                       std::string(option.name) + "\"";
            }
            return std::string();

        case OPTION_CONSTANT_IDENTIFIER:
            if (value_kind != OPTION_CONSTANT_IDENTIFIER) {
                return "Value must be identifier for enum-valued option \"" +
                       std::string(option.name) + "\"";
            }
            if (!names_contain(option.enum_values, text)) {
                return "Option \"" + std::string(option.name) +
                       "\" has no enum value named \"" + text + "\"";
            }
            break;

        case OPTION_CONSTANT_AGGREGATE:
            if (value_kind != OPTION_CONSTANT_AGGREGATE) {
                return "Option \"" + std::string(option.name) +
                       "\" is a message; set it with an aggregate value "
                       "{ <proto text format> }";
            }
            return std::string();

        default:
            return std::string();
    }

    // The value-dependent rules. They only apply to a field-scoped option.
    if (option.owner != OPTION_OWNER_FIELD) return std::string();

    const std::string option_name(option.name);
    if (option_name == "jstype") {
        // `ValidateJSType` refuses a declared kind outside the 64-bit
        // integers (`descriptor.cc:8583-8617`). JS_NORMAL is the option's
        // default and every field accepts it (U-08), so the rule keys on the
        // value, not on presence. An unresolved name is not a 64-bit integer
        // either.
        if (text != "JS_NORMAL" &&
            (!field.has_type || !is_64_bit_integer(field.type))) {
            return "jstype is only allowed on int64, uint64, sint64, fixed64 "
                   "or sfixed64 fields";
        }
    }
    if (option_name == "lazy" || option_name == "unverified_lazy") {
        // `ValidateFieldOptions` refuses a non-message field
        // (descriptor.cc:7968-7974). Either flag written as `false` is the
        // default and every field accepts it (U-08); only `true` reaches the
        // rule. A named type's kind is unknown until linking, so that half is
        // decided by the shared semantic pass, which records the option's
        // location in the field provenance.
        if (text == "true" &&
            !field.named && (!field.has_type ||
                             field.type != FieldDescriptorProto::TYPE_MESSAGE)) {
            return "[lazy = true] can only be specified for submessage fields";
        }
    }
    return std::string();
}

} // namespace easypb_proto
