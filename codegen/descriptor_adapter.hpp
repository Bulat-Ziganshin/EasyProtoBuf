#ifndef EASYPB_DESCRIPTOR_ADAPTER_HPP_INCLUDED
#define EASYPB_DESCRIPTOR_ADAPTER_HPP_INCLUDED

#include <string>
#include <vector>

#include "schema.hpp"

namespace easypb_schema {

// Append named, decoded files; take owned copies and do not select targets.
//
// The borrowed input may be discarded as soon as this call returns: every
// retained string is deep-copied into the owning file's pool, so the rule
// holds for the file, package, syntax and import names, message/field names
// and type names, defaults, enum values, oneofs, service and method names,
// the `(easypb.cpp).type` custom template, and every presence flag. The
// descriptors must stay alive only for the duration of the call.
//
// `diagnostic_label` names the source (a `.pbs` container path, or a future
// plugin request name) in every diagnostic this function reports. The label
// is a diagnostic identity, not a logical file name, and no operation
// requires it to exist on disk.
//
// Each file must carry a non-empty `name`: dependencies and target selection
// both address files by their logical names, and one named descriptor is the
// smallest unit this API accepts. The anonymous single-file `.pbs`
// compatibility path uses append_unnamed_descriptor below instead. The logical
// name is validated with the shared logical-path rule before anything is
// appended (V-18): relative `/`-separated UTF-8, no absolute or drive
// prefix, no backslashes or quotes, no `.`/`..` components, no control
// characters (DEL included, V-35), and at most the shared component bound. The
// same rule the output publisher applies, so a descriptor-set input name can
// no longer pass on stdout and fail under `--out-dir`.
//
// Every field label must be one of the three enumerators descriptor.proto
// declares (LABEL_OPTIONAL, LABEL_REQUIRED, LABEL_REPEATED); an out-of-range
// value is refused with a diagnostic that names the field and the value
// (V-42). protoc's decoder routes such a value to the unknown fields - the
// descriptor then has no label - so this is deliberate strictness: the model
// must not carry a value the generator ignores but the merge would compare.
//
// Duplicate logical names are rejected both within one call and against
// every file appended earlier, with DIAGNOSTIC_DUPLICATE_SYMBOL naming the
// duplicate. On any failure the whole result is cleared, so a caller must
// not keep views into it across a failed ingestion; callers append all
// inputs before linking or freezing the set.
bool append_descriptor_files(const std::vector<FileDescriptorProto>& files,
                             const std::string& diagnostic_label,
                             SchemaSet& result, Diagnostic& error);

// The logical-name rule of V-18, for one decoded file. `error.file` is
// `diagnostic_label` (the container path) and the message names the rejected
// spelling; an empty name is the caller's decision, because only the named
// ingestion path requires one. The descriptor-set loader and the print mode
// call this before they compare or print anything, so the verdict does not
// depend on which input mode reads the bytes.
bool validate_descriptor_file_name(str_view name,
                                   const std::string& diagnostic_label,
                                   Diagnostic& error);

// The V-42 label rule, walked over every message field and extension member of
// one decoded file. Returns false with a diagnostic naming the field, its
// message scope and the rejected value when a label is outside the three
// `descriptor.proto` enumerators. `diagnostic_label` becomes `error.file`, the
// same convention append_descriptor_files uses. The walk is recursive over
// nested messages, so the caller must have enforced the message-nesting bound
// first (`check_descriptor_set_nesting` on a container, or the adapter's own
// check); both ingestion paths and the print mode do.
bool validate_descriptor_field_labels(const FileDescriptorProto& file,
                                     const std::string& diagnostic_label,
                                     Diagnostic& error);

// Append exactly one descriptor that has no logical name: the legacy
// single-file `.pbs` workflow. `identity` is the container path; it becomes
// physical_name and labels a diagnostic, but it is never interned into the
// descriptor and never inserted into the set's name index (W-57, W-18). The
// same deep copy and the same nesting bound as the named path apply.
bool append_unnamed_descriptor(const FileDescriptorProto& file,
                               const std::string& identity, SchemaSet& result,
                               Diagnostic& error);

// True when two decoded files carry the same modeled content: every field
// descriptor.pb.hpp declares, read at its effective value. Where the absence
// of a field changes what the linker reads, presence is content too (an absent
// kind is refused, an absent oneof index means "not in a oneof"); where it only
// spells the protobuf default, the effective value decides instead: an absent
// label is `LABEL_OPTIONAL`, `packed` is the value the emitter writes
// (`effective_packed`), an absent range endpoint is 0, and a default value is
// the canonical text the adapter stores. A `cpp` group without its `type`
// member is no custom type; a present-but-empty `type` stays distinct because
// generation (and the source parser) refuse it. The presence bits of `name` and
// `package` are not modeled identity (W-48): ingest normalizes them, and the
// strings are compared instead. For the unsupported syntax class the edition
// carrier is compared too, so a class read from `edition` and one read from
// `syntax` are not one file (U-52).
//
// The descriptor loader uses this for the overlap rule of `W-36` - a file two
// containers both carry is merged when the two copies agree, and refused as a
// conflict when they do not - and the rule is about the content this project
// reads, not about the two containers' bytes.
bool same_descriptor_file(const FileDescriptorProto& left,
                          const FileDescriptorProto& right);

} // namespace easypb_schema

#endif
