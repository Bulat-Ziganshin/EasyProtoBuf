#include "parser_benchmark.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "proto_parser.hpp"

namespace easypb_proto {
namespace {

struct DescriptorStats
{
    std::size_t message_types;
    std::size_t fields;
    std::size_t map_entries;
    std::size_t map_entry_fields;

    DescriptorStats()
        : message_types(0), fields(0), map_entries(0), map_entry_fields(0) {}
};

std::string format_diagnostic(const Diagnostic& diagnostic)
{
    std::ostringstream result;
    if (!diagnostic.file.empty()) result << diagnostic.file;
    if (diagnostic.location.line != 0) {
        if (!diagnostic.file.empty()) result << ':';
        result << diagnostic.location.line << ':' << diagnostic.location.column;
    }
    if (result.tellp() > 0) result << ": ";
    if (diagnostic.warning) result << "warning: ";
    result << diagnostic.message;
    return result.str();
}

bool read_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    contents = buffer.str();
    return true;
}

bool is_map_entry(const DescriptorProto& message)
{
    return message.has_options && message.options.has_map_entry &&
           message.options.map_entry;
}

void add_message_stats(const DescriptorProto& message, DescriptorStats& stats)
{
    ++stats.message_types;
    stats.fields += message.field.size();
    if (is_map_entry(message)) {
        ++stats.map_entries;
        stats.map_entry_fields += message.field.size();
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        add_message_stats(message.nested_type[i], stats);
    }
}

void add_file_stats(const FileDescriptorProto& file, DescriptorStats& stats)
{
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        add_message_stats(file.message_type[i], stats);
    }
}

bool parse_one(const std::string& path,
               const std::string& source,
               ParsedProto& parsed,
               bool report_warnings,
               std::ostream& errors)
{
    Diagnostic error;
    if (!parse_proto(path, source.data(), source.size(), parsed, error)) {
        errors << format_diagnostic(error) << '\n';
        return false;
    }
    if (report_warnings) {
        for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
            errors << format_diagnostic(parsed.warnings[i]) << '\n';
        }
    }
    return true;
}

} // namespace

int run_parser_benchmark(const std::vector<std::string>& paths,
                         unsigned minimum_milliseconds,
                         std::ostream& output,
                         std::ostream& errors)
{
    if (paths.empty()) {
        errors << "benchmark requires at least one .proto file\n";
        return 2;
    }

    std::vector<std::string> sources(paths.size());
    std::uint64_t bytes_per_round = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!read_file(paths[i], sources[i])) {
            errors << paths[i] << ": cannot read file\n";
            return 2;
        }
        bytes_per_round += static_cast<std::uint64_t>(sources[i].size());
    }

    DescriptorStats stats;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        ParsedProto parsed;
        if (!parse_one(paths[i], sources[i], parsed, true, errors)) return 1;
        add_file_stats(parsed.file, stats);
    }

    const double minimum_seconds =
        static_cast<double>(minimum_milliseconds) / 1000.0;
    std::uint64_t measured_rounds = 0;
    ParsedProto scratch;
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished;
    double seconds = 0.0;

    do {
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (!parse_one(paths[i], sources[i], scratch, false, errors)) return 1;
        }
        ++measured_rounds;
        finished = std::chrono::steady_clock::now();
        seconds = std::chrono::duration_cast<std::chrono::duration<double> >(
                      finished - started).count();
    } while (seconds < minimum_seconds);

    const std::uint64_t measured_bytes = bytes_per_round * measured_rounds;
    const std::uint64_t file_parses =
        measured_rounds * static_cast<std::uint64_t>(paths.size());
    const double megabytes_per_second =
        seconds > 0.0 ? static_cast<double>(measured_bytes) / seconds / 1000000.0
                      : 0.0;

    output << "Parsed " << measured_bytes << " input bytes in "
           << std::fixed << std::setprecision(6) << seconds << " s ("
           << std::setprecision(2) << megabytes_per_second << " MB/s)\n";
    output << "Files: " << paths.size() << '\n';
    output << "Bytes per round: " << bytes_per_round << '\n';
    output << "Measured rounds: " << measured_rounds << '\n';
    output << "File parses: " << file_parses << '\n';
    output << "Warm-up rounds: 1 (not measured)\n";
    output << "Message types: " << stats.message_types
           << " (including " << stats.map_entries
           << " synthetic map entries)\n";
    output << "Fields: " << stats.fields
           << " (including " << stats.map_entry_fields
           << " synthetic map-entry fields)\n";
    return 0;
}

} // namespace easypb_proto
