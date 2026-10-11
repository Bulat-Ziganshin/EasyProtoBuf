#include "generation_options.hpp"

namespace easypb_codegen {

bool finalize_generation_options(GenerationOptions& options, std::string& error)
{
    // A required field's presence cannot be checked without its has_* field,
    // so suppressing the latter suppresses the check as well.
    if (options.no_has_fields) {
        options.no_required = true;
    }
    if (options.cpp_repeated_type.find("{}") == std::string::npos &&
        options.cpp_repeated_type.find("{0}") == std::string::npos) {
        options.cpp_repeated_type += "<{}>";
    }
    if (options.cpp_map_type.find("{}") == std::string::npos &&
        options.cpp_map_type.find("{0}") == std::string::npos &&
        options.cpp_map_type.find("{1}") == std::string::npos) {
        options.cpp_map_type += "<{0},{1}>";
    }
    if (options.packed && options.no_packed) {
        error = "Options --packed and --no-packed can't be used together";
        return false;
    }
    return true;
}

} // namespace easypb_codegen
