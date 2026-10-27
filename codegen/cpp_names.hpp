#ifndef EASYPB_CODEGEN_CPP_NAMES_HPP_INCLUDED
#define EASYPB_CODEGEN_CPP_NAMES_HPP_INCLUDED

#include <set>
#include <string>

#include "descriptor.pb.hpp"

namespace easypb_codegen {

// Convert an absolute Protobuf name such as .foo.bar.Outer.Inner to the
// corresponding absolute C++ name ::foo::bar::Outer::Inner. Every component
// that cannot be spelled verbatim in C++ is escaped (see cpp_identifier).
std::string cpp_absolute_name(const str_view& absolute_proto_name);

// The C++ name a Protobuf declaration name maps to. A name that is a C++
// keyword - or one of the two macro names the reference compiler escapes as
// well - gets an underscore appended; a name that is not a C++ identifier at
// all is refused, because no suffix can repair it (a field named "0Zc" cannot
// become a C++ member); every other name is returned unchanged.
std::string cpp_identifier(const std::string& name);

// Convert an absolute Protobuf name to the spelling used inside the current
// package namespace. Only an exact package-component prefix is stripped;
// names from other packages remain absolute C++ references. Every component
// goes through cpp_identifier, so the same-package spelling and the absolute
// spelling of one declaration agree (CR-43).
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

// The global names the generated header's own preamble puts into the global
// namespace. FILE_TEMPLATE includes <cstdint> directly and <easypb.hpp>, whose
// includes name <cstdint> and <cstddef> (include/easypb.hpp:13-14), so the C
// typedefs of those two headers are visible to every generated header. A
// declaration or a first package component with one of these names is spelled
// next to a typedef the header itself uses: `struct int32_t` and `namespace
// int32_t` are both ill-formed (`using typedef-name 'int32_t' after 'struct'`),
// so the name is an occupant of the file scope and a refused package
// component. The list is the family the preamble provides, enumerated by the
// header that declares it, not the handful of names a bug report happened to
// try.
const std::set<std::string>& cpp_global_names();

} // namespace easypb_codegen

#endif
