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
// compatibility path uses append_unnamed_descriptor below instead. Logical-name
// spelling rules (relative path, no quotes or control bytes) are not checked
// here: they are enforced where a name becomes a generated output path.
//
// Duplicate logical names are rejected both within one call and against
// every file appended earlier, with DIAGNOSTIC_DUPLICATE_SYMBOL naming the
// duplicate. On any failure the whole result is cleared, so a caller must
// not keep views into it across a failed ingestion; callers append all
// inputs before linking or freezing the set.
bool append_descriptor_files(const std::vector<FileDescriptorProto>& files,
                             const std::string& diagnostic_label,
                             SchemaSet& result, Diagnostic& error);

// Append exactly one descriptor that has no logical name: the legacy
// single-file `.pbs` workflow. `identity` is the container path; it becomes
// physical_name and labels a diagnostic, but it is never interned into the
// descriptor and never inserted into the set's name index (W-57, W-18). The
// same deep copy and the same nesting bound as the named path apply.
bool append_unnamed_descriptor(const FileDescriptorProto& file,
                               const std::string& identity, SchemaSet& result,
                               Diagnostic& error);

// True when two decoded files carry the same modeled content: every field
// descriptor.pb.hpp declares. Where the absence of a field changes what the
// linker reads, presence is content too (an absent kind is refused, an absent
// label is OPTIONAL, an absent oneof index means "not in a oneof"); where it
// only spells the protobuf default, the effective value decides instead
// (CR-24: absent/`"proto2"` syntax, `packed = false`, an empty option message,
// a derived `json_name`). The presence bits of `name` and `package` are not
// modeled identity (W-48): ingest normalizes them, and the strings are
// compared instead. The descriptor loader uses this for the overlap rule of
// `W-36` - a file two containers both carry is merged when the two copies
// agree, and refused as a conflict when they do not - and the rule is about
// the content this project reads, not about the two containers' bytes.
bool same_descriptor_file(const FileDescriptorProto& left,
                          const FileDescriptorProto& right);

} // namespace easypb_schema

#endif
