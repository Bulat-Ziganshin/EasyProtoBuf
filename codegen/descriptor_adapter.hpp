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
// compatibility path belongs to descriptor_loader.hpp, which reuses the same
// copy helper with an internal identity instead. Logical-name spelling rules
// (relative path, no quotes or control bytes) are not checked here: they are
// enforced where a name becomes a generated output path.
//
// Duplicate logical names are rejected both within one call and against
// every file appended earlier, with DIAGNOSTIC_DUPLICATE_SYMBOL naming the
// duplicate. On any failure the whole result is cleared, so a caller must
// not keep views into it across a failed ingestion; callers append all
// inputs before linking or freezing the set.
bool append_descriptor_files(const std::vector<FileDescriptorProto>& files,
                             const std::string& diagnostic_label,
                             SchemaSet& result, Diagnostic& error);

} // namespace easypb_schema

#endif
