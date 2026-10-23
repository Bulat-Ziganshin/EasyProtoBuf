#include "header_paths.hpp"

#include <cstddef>
#include <stdexcept>

#include "logical_paths.hpp"

namespace easypb_codegen {

std::string generated_header_path(const std::string& logical_name)
{
    std::string reason;
    if (!easypb_schema::validate_logical_path(logical_name, reason)) {
        throw std::runtime_error("logical file name \"" + logical_name +
                                 "\" cannot be mapped to a header path: " +
                                 reason);
    }

    // The reference compiler's order (compiler/code_generator_lite.cc:41-48):
    // a final `.protodevel` is stripped first, otherwise a final `.proto`.
    // One strip, no loop: `legacy.protodevel.proto` loses only `.proto` and
    // `legacy.proto.protodevel` loses only `.protodevel`. The logical name
    // itself keeps the suffix (`// Source:` prints it, W-13); only the output
    // name is stripped (W-25).
    const std::string protodevel_suffix = ".protodevel";
    const std::string proto_suffix = ".proto";
    const std::string header_suffix = ".pb.hpp";
    if (logical_name.size() >= protodevel_suffix.size() &&
        logical_name.compare(logical_name.size() - protodevel_suffix.size(),
                             protodevel_suffix.size(),
                             protodevel_suffix) == 0) {
        return logical_name.substr(
                   0, logical_name.size() - protodevel_suffix.size()) +
               header_suffix;
    }
    if (logical_name.size() >= proto_suffix.size() &&
        logical_name.compare(logical_name.size() - proto_suffix.size(),
                             proto_suffix.size(), proto_suffix) == 0) {
        return logical_name.substr(
                   0, logical_name.size() - proto_suffix.size()) +
               header_suffix;
    }
    return logical_name + header_suffix;
}

} // namespace easypb_codegen
