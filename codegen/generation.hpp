#ifndef EASYPB_CODEGEN_GENERATION_HPP_INCLUDED
#define EASYPB_CODEGEN_GENERATION_HPP_INCLUDED

#include <string>

#include "descriptor.pb.hpp"
#include "generation_options.hpp"

namespace easypb_codegen {

// Generate one complete header from a single decoded descriptor file.
//
// This is the extraction's entry point. The linked entry point frozen by the
// plan (`generate_header(const SchemaFile&, const LinkedSchema&, const
// GenerationOptions&)`) replaces it once the descriptor adapter lands; until
// then the standalone adapter calls this function and passes the input path as
// `provenance`, which preserves the pre-extraction output byte for byte.
//
// The result owns its text and keeps no view into `file`; common code never
// prints, never selects a stream and never exits.
std::string generate_descriptor_header(const FileDescriptorProto& file,
                                       const std::string& provenance,
                                       const GenerationOptions& options);

} // namespace easypb_codegen

#endif
