#ifndef EASYPB_PRETTY_PRINTER_HPP_INCLUDED
#define EASYPB_PRETTY_PRINTER_HPP_INCLUDED

#include <iosfwd>

#include "proto_parser.hpp"

namespace easypb_proto {

void print_descriptor(const FileDescriptorProto& file, std::ostream& output);
void print_descriptor(const ParsedProto& parsed, std::ostream& output);

} // namespace easypb_proto

#endif
