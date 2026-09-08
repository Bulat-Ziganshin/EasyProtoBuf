#ifndef EASYPB_CODEGEN_CPP_NAMES_HPP_INCLUDED
#define EASYPB_CODEGEN_CPP_NAMES_HPP_INCLUDED

#include <string>

namespace easypb_codegen {

// Convert an absolute Protobuf name such as .foo.bar.Outer.Inner to the
// corresponding absolute C++ name ::foo::bar::Outer::Inner.
std::string cpp_absolute_name(const std::string& absolute_proto_name);

// Convert an absolute Protobuf name to the spelling used inside the current
// package namespace. Only an exact package-component prefix is stripped;
// names from other packages remain absolute C++ references.
std::string cpp_local_name(const std::string& absolute_proto_name,
                           const std::string& package);

// Return the complete insertion-macro stem using the established legacy
// spelling. Package components are omitted and lexical message nesting is
// flattened with underscores: .foo.bar.Outer.Inner -> EASYPB_Outer_Inner.
std::string insertion_macro_stem(const std::string& absolute_proto_name,
                                 const std::string& package);

// Validate that every Protobuf package component can be emitted verbatim as a
// C++ namespace name. The empty package is valid and means global namespace.
void validate_cpp_package(const std::string& package);

} // namespace easypb_codegen

#endif
