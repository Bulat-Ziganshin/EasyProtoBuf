#include "descriptor_adapter.hpp"

#include <cstddef>
#include <set>
#include <sstream>
#include <string>

#include "schema_semantics.hpp"

namespace easypb_schema {
namespace {

// One string into the owning pool. An empty value stays an empty view:
// saving it would allocate a block for nothing, and no reader distinguishes
// an interned empty string from an absent one.
str_view intern_text(StringPool& strings, str_view value)
{
    if (value.empty()) return str_view();
    return strings.save(value.data(), value.size());
}

// The FIELD kinds whose default text is an integer literal. bool, enum,
// string and bytes carry their own spellings and must not be touched.
bool integral_default_type(int type)
{
    switch (type) {
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_FIXED32:
        case FieldDescriptorProto::TYPE_FIXED64:
        case FieldDescriptorProto::TYPE_SFIXED32:
        case FieldDescriptorProto::TYPE_SFIXED64:
            return true;
        default:
            return false;
    }
}

// The text a default value is stored and compared by. Floating-point and
// integral defaults are re-rendered from their parsed value, because the
// reference compiler and this project's source parser both store that form
// (W-04, CR-02, CR-17); `0x0A`, `012` and `10` are therefore one value, not
// three. The refusal spellings `'+'` and leading ASCII whitespace are never
// rewritten: they are diagnostics (W-03, W-40), and normalizing them would
// move the refusal behind a rewritten value. Every other family keeps its
// stored spelling, because that spelling is what the emitter writes.
std::string canonical_default_text(const FieldDescriptorProto& field)
{
    const std::string raw = view_text(field.default_value);
    if (!field.has_type) return raw;
    std::string normalized;
    if (field.type == FieldDescriptorProto::TYPE_FLOAT ||
        field.type == FieldDescriptorProto::TYPE_DOUBLE) {
        if (can_normalize_default_text(raw) &&
            canonical_float_default(
                raw, field.type == FieldDescriptorProto::TYPE_FLOAT,
                normalized)) {
            return normalized;
        }
        return raw;
    }
    if (integral_default_type(field.type) &&
        can_normalize_default_text(raw) &&
        canonical_integral_default(raw, normalized)) {
        return normalized;
    }
    return raw;
}

void intern_enum(StringPool& strings, EnumDescriptorProto& enum_type)
{
    enum_type.name = intern_text(strings, enum_type.name);
    for (std::size_t i = 0; i < enum_type.value.size(); ++i) {
        enum_type.value[i].name =
            intern_text(strings, enum_type.value[i].name);
    }
    // CR-06: every `reserved_name` is a retained string like the rest. The
    // walk is closed over descriptor.pb.hpp, so a field added to the model
    // without a line here keeps pointing into the caller buffer.
    for (std::size_t i = 0; i < enum_type.reserved_name.size(); ++i) {
        enum_type.reserved_name[i] =
            intern_text(strings, enum_type.reserved_name[i]);
    }
}

void intern_message(StringPool& strings, DescriptorProto& message)
{
    message.name = intern_text(strings, message.name);
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        FieldDescriptorProto& field = message.field[i];
        field.name = intern_text(strings, field.name);
        field.type_name = intern_text(strings, field.type_name);
        field.default_value = intern_text(strings, field.default_value);
        field.json_name = intern_text(strings, field.json_name);
        field.options.cpp.type = intern_text(strings, field.options.cpp.type);

        // W-04: the reference compiler's descriptor builder rewrites the
        // out-of-range spellings `strtod` reports, so a hand-written `.pbs`
        // carrying `1e400`, `1e-400` or a finite subnormal is re-serialized
        // as the canonical text `inf`, `0`, `-0` or the subnormal decimal.
        // CR-02: the same canonicalizer owns every spelling the reference
        // compiler accepts, including `INF`, `Infinity`, `NAN` and
        // `nan(payload)` (its `io::NoLocaleStrtod` is case-insensitive and
        // reads the C99 payload shape). The gate is `can_normalize_default_text`
        // alone: a leading `+` (W-40) and a leading space (W-03) are refusals,
        // not spellings, so the adapter must not normalize them - that would
        // hide the diagnostic behind a rewritten value.
        //
        // The canonicalizer runs on every normalizable spelling, exactly as
        // the source parser runs it. An `is_float_text` gate used to decide
        // this and it split the two paths on MSVC: the CRTs disagree about
        // ERANGE for a finite subnormal (`strtod("1e-320")` sets ERANGE on
        // glibc and leaves errno clear on the MSVC CRT), so the descriptor
        // path kept the raw spelling while the source path stored the
        // canonical one, and the two generated headers stopped matching for
        // `defaults-float-overflow`. The canonical output is idempotent: the
        // text a protoc-written descriptor already carries comes back
        // unchanged, so `--print-descriptor` bytes do not move for any
        // accepted row.
        //
        // CR-17's descriptor half: protoc re-renders an integral default
        // from its parsed value (`descriptor.cc:2835`), so the adapter must do
        // the same or a hand-written `.pbs` carrying `0x0A` would emit a
        // different header from the source path. A text the shared parser
        // rejects keeps its spelling; validate_default owns the range and
        // grammar diagnostics. The merge identity reads the same canonical
        // text (U-07), so the two copies of one file cannot disagree about a
        // default only because one container spelled it differently.
        if (field.has_default_value) {
            const std::string canonical = canonical_default_text(field);
            if (canonical != view_text(field.default_value)) {
                field.default_value = strings.save(canonical);
            }
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        intern_message(strings, message.nested_type[i]);
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        intern_enum(strings, message.enum_type[i]);
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        message.oneof_decl[i].name =
            intern_text(strings, message.oneof_decl[i].name);
    }
    // CR-06: the message's `reserved_name` entries are retained strings too.
    for (std::size_t i = 0; i < message.reserved_name.size(); ++i) {
        message.reserved_name[i] =
            intern_text(strings, message.reserved_name[i]);
    }
}

// Every str_view the descriptor model retains, in one walk. The set is
// closed over descriptor.pb.hpp: a field added there without a line here
// would keep pointing into a caller buffer this library does not own, so
// the D2 lifetime test enumerates the same list.
void intern_file_descriptor(StringPool& strings, FileDescriptorProto& file)
{
    file.name = intern_text(strings, file.name);
    file.package = intern_text(strings, file.package);
    file.syntax = intern_text(strings, file.syntax);
    for (std::size_t i = 0; i < file.dependency.size(); ++i) {
        file.dependency[i] = intern_text(strings, file.dependency[i]);
    }
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        intern_message(strings, file.message_type[i]);
    }
    for (std::size_t i = 0; i < file.enum_type.size(); ++i) {
        intern_enum(strings, file.enum_type[i]);
    }
    for (std::size_t i = 0; i < file.service.size(); ++i) {
        ServiceDescriptorProto& service = file.service[i];
        service.name = intern_text(strings, service.name);
        for (std::size_t j = 0; j < service.method.size(); ++j) {
            MethodDescriptorProto& method = service.method[j];
            method.name = intern_text(strings, method.name);
            method.input_type = intern_text(strings, method.input_type);
            method.output_type = intern_text(strings, method.output_type);
        }
    }
}

// The one shared deep-copy helper: the tree is copied whole, then every
// retained string is re-pointed into the owning pool. A shallow assignment
// would leave the copy pointing into the caller's buffer, which is exactly
// the defect this helper exists to prevent (D1/D2).
//
// `has_name` is the ingestion path's name rule. The named path is normalized
// here so the bit always agrees with the required non-empty text (W-48); the
// anonymous single-file path passes false and owns no logical name at all
// (W-57) - a decoded descriptor may carry a present-but-empty name field, so
// clearing the bit after the assignment is the only correct spelling.
void copy_descriptor(const FileDescriptorProto& source, SchemaFile& slot,
                     bool has_name = true)
{
    slot.file = source;
    intern_file_descriptor(slot.strings, slot.file);
    // Presence of a file's name and package is not modeled information: the
    // name is required and non-empty (validated before this copy), and a
    // non-empty package is a package whatever bit the container carried
    // (W-48). Normalizing here, at the one deep copy every descriptor path
    // shares, makes the linker and the generator read one shape. A caller's
    // hand-built descriptor may leave the bit false while carrying the text;
    // the wire decoder sets it, so the no-op direction is the one `.pbs` bytes
    // take.
    //
    // CR-45: the bit is assigned, never only set. An empty package is no
    // package (protoc does not call `AddPackage` for an empty name,
    // descriptor.cc:5372-5376), so a container that spells the field present
    // with an empty value must not keep `has_package == true` - otherwise a
    // library caller that inspects the bit (or a future reader that trusts it
    // alone) sees a package that does not exist. `has_name` already had this
    // shape.
    slot.file.has_name = has_name;
    slot.file.has_package = !slot.file.package.empty();
}

// True when the byte is a C0 control character. A logical Protobuf file name
// never carries one: the name is a path the user spelled, and the generator
// writes it into the "// Source:" line of the header, so a newline in it
// would inject arbitrary lines into the generated file.
bool is_control_character(char c)
{
    const unsigned char byte = static_cast<unsigned char>(c);
    return byte < 0x20u;
}

// The control character the name carries, spelled for a diagnostic.
std::string control_character_name(char c)
{
    const unsigned char byte = static_cast<unsigned char>(c);
    switch (byte) {
        case 0x00: return "NUL";
        case 0x09: return "TAB";
        case 0x0A: return "LF";
        case 0x0D: return "CR";
        default: break;
    }
    std::ostringstream text;
    text << "0x" << std::hex << static_cast<unsigned>(byte);
    return text.str();
}

bool fail(const std::string& label, const std::string& message,
          DiagnosticCode code, SchemaSet& result, Diagnostic& error)
{
    result.clear();
    error = Diagnostic();
    error.file = label;
    error.location = unknown_location();
    error.message = message;
    error.code = code;
    return false;
}

// --- structural equality of two modeled descriptors -------------------------
//
// `W-36` makes two containers that carry the same logical file a merge instead
// of a blanket refusal. "The same file" is decided over the modeled content:
// every field descriptor.pb.hpp declares, read at its EFFECTIVE value. Where
// the ABSENCE of a field has a defined meaning for the linker, presence is
// content too (an absent kind is refused by W-10, an absent oneof index means
// "not in a oneof"). Where absence is only the protobuf default, the value the
// linker and the emitter read decides, so a re-encoded set and its source stay
// one file (CR-24, U-07): an absent label is `LABEL_OPTIONAL`, an absent
// `packed` option is the schema default (`effective_packed`), an absent range
// endpoint is 0, a `cpp` group without its `type` member is no type override,
// and a default value is compared as the canonical text the adapter stores.
// The file's `name` and `package` are compared by value; their presence bits
// are normalized at ingest (W-48) and are not modeled identity. Strings compare
// by value, so the comparison is independent of which pool owns them.
//
// The walk must grow with the model: a field added to descriptor.pb.hpp and
// not compared here would make two different files look identical. The
// unit suite pins that with the identity fixture of the loader lane.

// An absent label is LABEL_OPTIONAL. The raw member defaults to 0, which is
// not a label at all, so the raw value and the presence bit cannot decide
// identity (U-07).
int effective_label(const FieldDescriptorProto& field)
{
    return field.has_label ? field.label : FieldDescriptorProto::LABEL_OPTIONAL;
}

// A custom type is a request only when the extension group AND its `type`
// member are present; otherwise the emitter applies the global option
// (`cpp_type_as_str`), so a `cpp` group without `type` is the unset request
// (U-07). A present-but-empty `type` is NOT the unset request: both the source
// parser and the emitter refuse that spelling, and treating it as absent would
// make the pair's verdict depend on the argument order while opening a
// descriptor-only door the source path closes.
bool same_cpp_type(const FieldOptions& left, const FieldOptions& right)
{
    const bool left_request = left.has_cpp && left.cpp.has_type;
    const bool right_request = right.has_cpp && right.cpp.has_type;
    if (left_request != right_request) return false;
    if (!left_request) return true;
    return left.cpp.type == right.cpp.type;
}

// The effective JSON name: the written one when the descriptor carries it, the
// derived name when it does not. A written name equal to the derived one is the
// same value either way, so only a custom spelling changes the answer. protoc
// writes `json_name` for every field of a descriptor set it produced; a
// hand-encoded copy may omit the field, and that copy still names the same JSON
// member (CR-24).
std::string effective_json_name(const FieldDescriptorProto& field)
{
    const std::string derived = to_json_name(view_text(field.name));
    return field.has_json_name ? view_text(field.json_name) : derived;
}

bool same_descriptor(const FieldDescriptorProto& left,
                     const FieldDescriptorProto& right, FileSyntax syntax)
{
    return left.name == right.name && left.number == right.number &&
           effective_label(left) == effective_label(right) &&
           left.type == right.type &&
           left.type_name == right.type_name &&
           canonical_default_text(left) == canonical_default_text(right) &&
           left.oneof_index == right.oneof_index &&
           effective_json_name(left) == effective_json_name(right) &&
           // D1-A: an explicit `optional` in proto3 is presence, so a copy
           // that carries the flag and one that does not are two different
           // definitions of the field, exactly like a differing label. Unset
           // and an explicit false are the same value.
           (left.has_proto3_optional ? left.proto3_optional : false) ==
               (right.has_proto3_optional ? right.proto3_optional : false) &&
           same_cpp_type(left.options, right.options) &&
           // U-04: `packed` is compared at its EFFECTIVE value, exactly as the
           // emitter computes it; presence alone would accept a proto3 pair
           // whose wire format depends on argument order.
           effective_packed(left, syntax) == effective_packed(right, syntax) &&
           left.has_name == right.has_name &&
           left.has_number == right.has_number &&
           left.has_type == right.has_type &&
           left.has_type_name == right.has_type_name &&
           left.has_default_value == right.has_default_value &&
           left.has_oneof_index == right.has_oneof_index;
}

bool same_descriptor(const MessageOptions& left, const MessageOptions& right)
{
    // Both options are read as bools: unset is false.
    return (left.has_map_entry ? left.map_entry : false) ==
               (right.has_map_entry ? right.map_entry : false) &&
           (left.has_message_set_wire_format
                ? left.message_set_wire_format
                : false) ==
               (right.has_message_set_wire_format
                    ? right.message_set_wire_format
                    : false);
}

// A range endpoint is the stored value: an absent endpoint is the protobuf
// default 0, and the linker reads that value, not the bit. Two copies that
// spell one range with different presence are one file (U-07).
bool same_descriptor(const DescriptorProto_ExtensionRange& left,
                     const DescriptorProto_ExtensionRange& right)
{
    return left.start == right.start && left.end == right.end;
}

// CR-06: a reserved range is modeled content, so two copies of one logical
// file that disagree about it are two different definitions. The values decide;
// presence does not (U-07).
bool same_descriptor(const DescriptorProto_ReservedRange& left,
                     const DescriptorProto_ReservedRange& right)
{
    return left.start == right.start && left.end == right.end;
}

bool same_descriptor(const EnumDescriptorProto_EnumReservedRange& left,
                     const EnumDescriptorProto_EnumReservedRange& right)
{
    return left.start == right.start && left.end == right.end;
}

bool same_descriptor(const EnumValueDescriptorProto& left,
                     const EnumValueDescriptorProto& right)
{
    return left.name == right.name && left.number == right.number &&
           left.has_name == right.has_name && left.has_number == right.has_number;
}

bool same_descriptor(const EnumOptions& left, const EnumOptions& right)
{
    // `allow_alias` is read as a bool: unset is false.
    return (left.has_allow_alias ? left.allow_alias : false) ==
           (right.has_allow_alias ? right.allow_alias : false);
}

bool same_descriptor(const EnumDescriptorProto& left,
                     const EnumDescriptorProto& right)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        !same_descriptor(left.options, right.options) ||
        left.value.size() != right.value.size() ||
        left.reserved_range.size() != right.reserved_range.size() ||
        left.reserved_name.size() != right.reserved_name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.value.size(); ++i) {
        if (!same_descriptor(left.value[i], right.value[i])) return false;
    }
    for (std::size_t i = 0; i < left.reserved_range.size(); ++i) {
        if (!same_descriptor(left.reserved_range[i], right.reserved_range[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.reserved_name.size(); ++i) {
        if (left.reserved_name[i] != right.reserved_name[i]) return false;
    }
    return true;
}

bool same_descriptor(const OneofDescriptorProto& left,
                     const OneofDescriptorProto& right)
{
    return left.name == right.name && left.has_name == right.has_name;
}

bool same_descriptor(const DescriptorProto& left, const DescriptorProto& right,
                     FileSyntax syntax)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        left.extension_count != right.extension_count ||
        !same_descriptor(left.options, right.options) ||
        left.field.size() != right.field.size() ||
        left.nested_type.size() != right.nested_type.size() ||
        left.enum_type.size() != right.enum_type.size() ||
        left.oneof_decl.size() != right.oneof_decl.size() ||
        left.extension_range.size() != right.extension_range.size() ||
        left.reserved_range.size() != right.reserved_range.size() ||
        left.reserved_name.size() != right.reserved_name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.field.size(); ++i) {
        if (!same_descriptor(left.field[i], right.field[i], syntax)) return false;
    }
    for (std::size_t i = 0; i < left.nested_type.size(); ++i) {
        if (!same_descriptor(left.nested_type[i], right.nested_type[i], syntax)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.enum_type.size(); ++i) {
        if (!same_descriptor(left.enum_type[i], right.enum_type[i])) return false;
    }
    for (std::size_t i = 0; i < left.oneof_decl.size(); ++i) {
        if (!same_descriptor(left.oneof_decl[i], right.oneof_decl[i])) return false;
    }
    for (std::size_t i = 0; i < left.extension_range.size(); ++i) {
        if (!same_descriptor(left.extension_range[i], right.extension_range[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.reserved_range.size(); ++i) {
        if (!same_descriptor(left.reserved_range[i], right.reserved_range[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.reserved_name.size(); ++i) {
        if (left.reserved_name[i] != right.reserved_name[i]) return false;
    }
    return true;
}

bool same_descriptor(const MethodDescriptorProto& left,
                     const MethodDescriptorProto& right)
{
    // CR-48: the two streaming flags are modeled content now, so two copies of
    // one logical file that disagree about them are two different definitions.
    // They are read as bools (unset is false), like every option flag: an
    // absent `client_streaming` and a written `false` describe one method.
    return left.name == right.name && left.input_type == right.input_type &&
           left.output_type == right.output_type &&
           (left.has_client_streaming ? left.client_streaming : false) ==
               (right.has_client_streaming ? right.client_streaming : false) &&
           (left.has_server_streaming ? left.server_streaming : false) ==
               (right.has_server_streaming ? right.server_streaming : false) &&
           left.has_name == right.has_name &&
           left.has_input_type == right.has_input_type &&
           left.has_output_type == right.has_output_type;
}

bool same_descriptor(const ServiceDescriptorProto& left,
                     const ServiceDescriptorProto& right)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        left.method.size() != right.method.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.method.size(); ++i) {
        if (!same_descriptor(left.method[i], right.method[i])) return false;
    }
    return true;
}

} // namespace

// The syntax class the linker reads, compared by classification: an absent
// string, an empty one and an explicit `"proto2"` are one proto2 file to
// protoc, so they are one file here too (CR-24). An unsupported class keeps the
// deciding spelling in the comparison, because two different unsupported
// spellings are two different files - both are then refused by A25, not merged.
// U-52: the carrier is part of that spelling. A file whose class comes from the
// `edition` field and one whose class comes from the `syntax` string are not
// one input even when the stored edition VALUE happens to agree; without the
// presence bit the merge would silently keep one of them.
bool same_file_syntax(const FileDescriptorProto& left,
                      const FileDescriptorProto& right)
{
    const FileSyntax left_class = classify_file_syntax(left);
    const FileSyntax right_class = classify_file_syntax(right);
    if (left_class != right_class) return false;
    if (left_class != FILE_SYNTAX_UNSUPPORTED) return true;
    return left.has_edition == right.has_edition &&
           left.edition == right.edition && left.syntax == right.syntax;
}

bool same_descriptor_file(const FileDescriptorProto& left,
                          const FileDescriptorProto& right)
{
    // The presence bits of the file's name and package are not part of the
    // modeled identity (W-48): the strings themselves are compared below, and
    // ingest normalizes the bits, so a stored copy and an incoming copy of one
    // file must not look different here. `syntax`/`edition` are compared by
    // class (CR-24); every other presence bit still decides, because it
    // changes what the linker reads. The field walk receives the file's own
    // syntax class, because the effective `packed` default depends on it
    // (U-04).
    const FileSyntax syntax = classify_file_syntax(left);
    if (left.name != right.name || left.package != right.package ||
        !same_file_syntax(left, right) ||
        left.dependency.size() != right.dependency.size() ||
        left.public_dependency != right.public_dependency ||
        left.weak_dependency != right.weak_dependency ||
        left.message_type.size() != right.message_type.size() ||
        left.enum_type.size() != right.enum_type.size() ||
        left.service.size() != right.service.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.dependency.size(); ++i) {
        if (left.dependency[i] != right.dependency[i]) return false;
    }
    for (std::size_t i = 0; i < left.message_type.size(); ++i) {
        if (!same_descriptor(left.message_type[i], right.message_type[i],
                             syntax)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.enum_type.size(); ++i) {
        if (!same_descriptor(left.enum_type[i], right.enum_type[i])) return false;
    }
    for (std::size_t i = 0; i < left.service.size(); ++i) {
        if (!same_descriptor(left.service[i], right.service[i])) return false;
    }
    return true;
}

bool append_descriptor_files(const std::vector<FileDescriptorProto>& files,
                             const std::string& diagnostic_label,
                             SchemaSet& result, Diagnostic& error)
{
    if (files.empty()) {
        return fail(diagnostic_label,
                    "descriptor source \"" + diagnostic_label +
                        "\" contains no FileDescriptorProto",
                    DIAGNOSTIC_GENERIC, result, error);
    }

    // Validate every name before anything is appended, so a failed call
    // never leaves a partial batch behind.
    std::set<str_view> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const str_view& raw_name = files[i].name;
        if (raw_name.empty()) {
            return fail(diagnostic_label,
                        "descriptor file #" +
                            easypb::detail::number_to_string(
                                static_cast<uint64_t>(i)) +
                            " in \"" + diagnostic_label +
                            "\" has no logical name; only the single-file "
                            "compatibility path may ingest an unnamed "
                            "descriptor",
                        DIAGNOSTIC_GENERIC, result, error);
        }
        const std::string logical = view_text(raw_name);
        if (!names.insert(raw_name).second || result.find_file(logical) != 0) {
            return fail(diagnostic_label,
                        "duplicate logical file name \"" + logical +
                            "\" in descriptor source \"" + diagnostic_label +
                            "\" (a file with this name was already appended)",
                        DIAGNOSTIC_DUPLICATE_SYMBOL, result, error);
        }
        // The name is a logical Protobuf name, and it reaches the "// Source:"
        // line of the generated header. A control character in it would
        // inject arbitrary lines into that header, so the name is refused
        // here - at the same place the empty and the duplicate names are
        // refused, and before anything is appended, so a failed call never
        // leaves a partial batch behind.
        for (std::size_t position = 0; position < logical.size(); ++position) {
            if (!is_control_character(logical[position])) continue;
            return fail(diagnostic_label,
                        "logical file name \"" + logical +
                            "\" in descriptor source \"" + diagnostic_label +
                            "\" carries a control character (" +
                            control_character_name(logical[position]) +
                            " at position " +
                            easypb::detail::number_to_string(
                                static_cast<uint64_t>(position)) +
                            "); a logical name cannot contain one",
                        DIAGNOSTIC_GENERIC, result, error);
        }
        // The nesting of the messages the file declares is bounded here, before
        // the file is copied: the copy interns every string of the tree and
        // walks it recursively (intern_message below), so a descriptor that
        // nests deeper than the limit would overflow the stack inside the
        // copy - and the copy is what a library caller hands this project.
        // The rule is the reference compiler's own (descriptor.h:3045) and the
        // sentence is the one it uses.
        if (!easypb_schema::validate_message_nesting_depth(files[i], logical,
                                                            error)) {
            result.clear();
            return false;
        }
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string logical = view_text(files[i].name);
        SchemaFile& slot = result.add_file(logical);
        copy_descriptor(files[i], slot);
        // Descriptor origin is a property of how the bytes were obtained,
        // not of where a container happened to live; a file wrapper may
        // retain the container path in physical_name afterwards.
        slot.physical_name.clear();
        slot.from_descriptor_set = true;
    }
    return true;
}

bool append_unnamed_descriptor(const FileDescriptorProto& file,
                               const std::string& identity, SchemaSet& result,
                               Diagnostic& error)
{
    // The anonymous single-file `.pbs` shape: the container path is metadata,
    // never a logical name. The identity therefore never reaches
    // StringPool::save and never enters the name index (W-57, W-18); the copy
    // is the same deep copy the named path uses, with the name rule switched
    // off.
    if (!validate_message_nesting_depth(file, identity, error)) {
        result.clear();
        return false;
    }
    SchemaFile& slot = result.add_unnamed_file(identity);
    copy_descriptor(file, slot, false);
    slot.from_descriptor_set = true;
    return true;
}

} // namespace easypb_schema
