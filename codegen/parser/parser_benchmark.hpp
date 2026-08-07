#ifndef EASYPB_PARSER_BENCHMARK_HPP_INCLUDED
#define EASYPB_PARSER_BENCHMARK_HPP_INCLUDED

#include <iosfwd>
#include <string>
#include <vector>

namespace easypb_proto {

int run_parser_benchmark(const std::vector<std::string>& paths,
                         unsigned minimum_milliseconds,
                         std::ostream& output,
                         std::ostream& errors);

} // namespace easypb_proto

#endif
