#ifndef EASYPB_CODEGEN_EMITTER_ERROR_HPP_INCLUDED
#define EASYPB_CODEGEN_EMITTER_ERROR_HPP_INCLUDED

#include <map>
#include <stdexcept>
#include <string>

#include "schema.hpp"

namespace easypb_codegen {

// One emitter refusal, tied to the declaration of the target file it blames.
//
// The emitter reads a linked descriptor, not source text, so a refusal used to
// reach the caller as a bare `std::runtime_error`: the diagnostic named the
// file but carried no position, and the CLI printed it without line and
// column. Every refusal about a declaration now throws this type with the
// declaration's `DescriptorPath`, and `generate_files`/the CLI turn that path
// into the position the source parser recorded (U-30). A `.pbs` carries no
// source text, so the lookup stays unknown there and the diagnostic is the
// same one it printed before; a synthesized declaration (a generated codec, a
// runtime namespace, a global typedef) has no path and is never blamed for a
// collision it loses.
class EmitterError : public std::runtime_error
{
public:
    EmitterError(const easypb_schema::DescriptorPath& declaration_path,
                 const std::string& message)
        : std::runtime_error(message),
          declaration_path_(declaration_path)
    {
    }

    const easypb_schema::DescriptorPath& declaration_path() const
    {
        return declaration_path_;
    }

private:
    easypb_schema::DescriptorPath declaration_path_;
};


// The source position `file` recorded for `path`, or unknown_location() when
// the declaration has none (descriptor input, or a declaration the parser
// synthesized rather than read from source text).
inline easypb_schema::SourceLocation emitter_declaration_location(
    const easypb_schema::SchemaFile& file,
    const easypb_schema::DescriptorPath& path)
{
    const std::map<easypb_schema::DescriptorPath,
                   easypb_schema::SourceLocation>::const_iterator found =
        file.declaration_locations.find(path);
    if (found == file.declaration_locations.end()) {
        return easypb_schema::unknown_location();
    }
    return found->second;
}

} // namespace easypb_codegen

#endif
