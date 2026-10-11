#ifndef EASYPB_CODEGEN_GENERATION_OPTIONS_HPP_INCLUDED
#define EASYPB_CODEGEN_GENERATION_OPTIONS_HPP_INCLUDED

#include <string>

namespace easypb_codegen {

// Code-generation options affecting the generated code. The defaults live
// here, not only in the CLI parser, so a caller that never touches the CLI
// gets the same documented behavior.
struct GenerationOptions
{
    bool no_class = false;
    bool no_decoder = false;
    bool no_encoder = false;
    bool no_has_fields = false;
    bool no_required = false;
    bool no_default_values = false;
    bool packed = false;
    bool no_packed = false;
    bool allow_self_recursive_containers = false;
    std::string cpp_string_type = "::std::string";
    std::string cpp_repeated_type = "::std::vector";
    std::string cpp_map_type = "::std::map";
};

// Apply the derived behavior every frontend shares and reject contradictory
// option sets. Returns false and fills `error` for the caller to report; on
// success `options` is normalized (for example a repeated-type template
// without a placeholder gains "<{}>").
bool finalize_generation_options(GenerationOptions& options, std::string& error);

} // namespace easypb_codegen

#endif
