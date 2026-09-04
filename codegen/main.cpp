#ifndef EASYPB_CODEGEN_WITH_PROTO_PARSER
#define EASYPB_CODEGEN_WITH_PROTO_PARSER 1
#endif

#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "popl.hpp"
#include "codegen.cpp"

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
#include "parser/parser_benchmark.hpp"
#include "parser/pretty_printer.hpp"
#include "parser/proto_parser.hpp"
#endif

namespace {

enum Action {
    ACTION_GENERATE,
    ACTION_PRINT_DESCRIPTOR,
    ACTION_BENCHMARK_PARSER
};

enum InputFormat {
    INPUT_AUTO,
    INPUT_PROTO_SOURCE,
    INPUT_DESCRIPTOR_SET
};

struct CommandLine
{
    Action action;
    InputFormat input_format;
    unsigned benchmark_milliseconds;
    std::vector<std::string> filenames;
    bool exit_after_help;

    CommandLine()
        : action(ACTION_GENERATE), input_format(INPUT_AUTO),
          benchmark_milliseconds(100), exit_after_help(false) {}
};

const char* usage_text()
{
#if EASYPB_CODEGEN_WITH_PROTO_PARSER
    return
        "Generator of C++ code from a ProtoBuf schema\n"
        "  Usage: codegen [options] file.proto...\n"
        "         codegen --descriptor-set [options] file.pbs...\n"
        "         codegen --print-descriptor [--descriptor-set] file...\n"
        "         codegen --benchmark-parser [--benchmark-ms N] file.proto...\n";
#else
    return
        "Generator of C++ code from a compiled ProtoBuf descriptor set\n"
        "  Usage: codegen [--descriptor-set] [options] file.pbs...\n";
#endif
}

bool read_file(const std::string& filename, std::string& contents)
{
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

FileDescriptorProto decode_single_file_descriptor(const std::string& filename,
                                                   const std::string& contents)
{
    const FileDescriptorSet set = easypb::decode<FileDescriptorSet>(contents);
    if (set.file.empty()) {
        throw std::runtime_error(filename +
            ": descriptor set contains no FileDescriptorProto");
    }
    if (set.file.size() != 1) {
        std::ostringstream message;
        message << filename << ": descriptor set contains " << set.file.size()
                << " files; exactly one is required. Regenerate it without "
                   "protoc --include_imports";
        throw std::runtime_error(message.str());
    }
    return set.file[0];
}

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
std::string format_diagnostic(const easypb_proto::Diagnostic& diagnostic)
{
    std::ostringstream output;
    if (!diagnostic.file.empty()) output << diagnostic.file;
    if (diagnostic.location.line != 0) {
        if (!diagnostic.file.empty()) output << ':';
        output << diagnostic.location.line << ':' << diagnostic.location.column;
    }
    if (output.tellp() > 0) output << ": ";
    if (diagnostic.warning) output << "warning: ";
    output << diagnostic.message;
    return output.str();
}

bool has_unresolved_types(const easypb_proto::ParsedProto& parsed)
{
    for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
        if (parsed.warnings[i].code == easypb_proto::DIAGNOSTIC_UNRESOLVED_TYPE) {
            return true;
        }
    }
    return false;
}

void report_warnings(const easypb_proto::ParsedProto& parsed)
{
    for (std::size_t i = 0; i < parsed.warnings.size(); ++i) {
        std::cerr << format_diagnostic(parsed.warnings[i]) << '\n';
    }
}

bool parse_source_file(const std::string& filename,
                       const std::string& contents,
                       easypb_proto::ParsedProto& parsed)
{
    easypb_proto::Diagnostic error;
    if (!easypb_proto::parse_proto(
            filename, contents.data(), contents.size(), parsed, error)) {
        std::cerr << format_diagnostic(error) << '\n';
        return false;
    }
    report_warnings(parsed);
    return true;
}
#endif

CommandLine parse_cmdline(int argc, char** argv)
{
    using namespace popl;

#if !EASYPB_CODEGEN_WITH_PROTO_PARSER
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--print-descriptor" ||
            argument == "--benchmark-parser" ||
            argument == "--benchmark-ms" ||
            argument.find("--benchmark-ms=") == 0) {
            throw std::runtime_error(
                argument + " is unavailable because this codegen build has no .proto parser");
        }
    }
#endif

    CommandLine command;
    OptionParser parser("Allowed options");

    auto help_option  = parser.add<Switch>("h", "help", "produce help message");
    auto groff_option = parser.add<Switch>("", "groff", "produce groff formatted help message");
    auto bash_option  = parser.add<Switch>("", "bash", "produce bash completion script");
    auto descriptor_set_option = parser.add<Switch>(
        "", "descriptor-set", "read binary FileDescriptorSet input");

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
    auto print_option = parser.add<Switch>(
        "", "print-descriptor", "print descriptor tree instead of generating C++");
    auto benchmark_option = parser.add<Switch>(
        "", "benchmark-parser", "benchmark parsing one or more .proto files");
    int benchmark_ms = 100;
    auto benchmark_ms_option = parser.add<Value<int> >(
        "", "benchmark-ms", "minimum measured parser time in milliseconds",
        100, &benchmark_ms);
#endif

    auto no_class_option = parser.add<Switch>(
        "c", "no-class", "don't generate C++ struct", &option.no_class);
    auto no_decoder_option = parser.add<Switch>(
        "d", "no-decoder", "don't generate decoder", &option.no_decoder);
    auto no_encoder_option = parser.add<Switch>(
        "e", "no-encoder", "don't generate encoder", &option.no_encoder);
    auto no_has_option = parser.add<Switch>(
        "f", "no-has-fields", "don't generate has_* fields", &option.no_has_fields);
    auto no_required_option = parser.add<Switch>(
        "", "no-required", "ignore 'required' attribute", &option.no_required);
    auto no_defaults_option = parser.add<Switch>(
        "", "no-default-values", "ignore default values", &option.no_default_values);
    auto packed_option = parser.add<Switch>(
        "p", "packed", "make all repeated fields packed when allowed", &option.packed);
    auto no_packed_option = parser.add<Switch>(
        "", "no-packed", "make all repeated fields non-packed", &option.no_packed);
    auto allow_self_recursive_containers_option = parser.add<Switch>(
        "", "allow-self-recursive-containers",
        "allow direct self-recursive repeated/map message fields",
        &option.allow_self_recursive_containers);

    auto string_type_option = parser.add<Value<std::string> >(
        "s", "string-type", "C++ type for string/bytes fields",
        "std::string", &option.cpp_string_type);
    auto repeated_type_option = parser.add<Value<std::string> >(
        "r", "repeated-type", "C++ container type for repeated fields",
        "std::vector", &option.cpp_repeated_type);
    auto map_type_option = parser.add<Value<std::string> >(
        "m", "map-type", "C++ container type for map fields",
        "std::map", &option.cpp_map_type);

    parser.parse(argc, argv);

    // print auto-generated help message
    if (groff_option->is_set()) {
        GroffOptionPrinter printer(&parser);
        std::cout << printer.print();
        command.exit_after_help = true;
        return command;
    }
    if (bash_option->is_set()) {
        BashCompletionOptionPrinter printer(&parser, "codegen");
        std::cout << printer.print();
        command.exit_after_help = true;
        return command;
    }
    if (help_option->count() || parser.non_option_args().empty()) {
        std::cout << usage_text() << '\n' << parser << '\n';
        command.exit_after_help = true;
        return command;
    }

    command.filenames = parser.non_option_args();
    if (descriptor_set_option->is_set()) {
        command.input_format = INPUT_DESCRIPTOR_SET;
    }

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
    if (print_option->is_set() && benchmark_option->is_set()) {
        throw std::runtime_error(
            "Options --print-descriptor and --benchmark-parser can't be used together");
    }
    if (benchmark_option->is_set()) command.action = ACTION_BENCHMARK_PARSER;
    else if (print_option->is_set()) command.action = ACTION_PRINT_DESCRIPTOR;

    if (benchmark_ms_option->is_set()) {
        if (benchmark_ms < 100) {
            throw std::runtime_error("--benchmark-ms must be at least 100");
        }
        command.benchmark_milliseconds = static_cast<unsigned>(benchmark_ms);
    }
    if (command.action != ACTION_BENCHMARK_PARSER && benchmark_ms_option->is_set()) {
        throw std::runtime_error(
            "--benchmark-ms is valid only with --benchmark-parser");
    }
    if (command.action == ACTION_BENCHMARK_PARSER &&
        command.input_format == INPUT_DESCRIPTOR_SET) {
        throw std::runtime_error(
            "--benchmark-parser accepts .proto source files, not --descriptor-set");
    }

    const bool generation_option_set =
        no_class_option->is_set() || no_decoder_option->is_set() ||
        no_encoder_option->is_set() || no_has_option->is_set() ||
        no_required_option->is_set() || no_defaults_option->is_set() ||
        packed_option->is_set() || no_packed_option->is_set() ||
        allow_self_recursive_containers_option->is_set() ||
        string_type_option->is_set() || repeated_type_option->is_set() ||
        map_type_option->is_set();
    if (command.action != ACTION_GENERATE && generation_option_set) {
        throw std::runtime_error(
            "code-generation options cannot be used with descriptor print or parser benchmark modes");
    }
#endif

    if (option.no_has_fields) {
        option.no_required = true;  // we can't check presence of a required field without employing the corresponding has_* field
    }
    if (option.cpp_repeated_type.find("{}") == std::string::npos &&
        option.cpp_repeated_type.find("{0}") == std::string::npos) {
        option.cpp_repeated_type += "<{}>";
    }
    if (option.cpp_map_type.find("{}") == std::string::npos &&
        option.cpp_map_type.find("{0}") == std::string::npos &&
        option.cpp_map_type.find("{1}") == std::string::npos) {
        option.cpp_map_type += "<{0},{1}>";
    }
    if (option.packed && option.no_packed) {
        throw std::runtime_error(
            "Options --packed and --no-packed can't be used together");
    }

    if (command.input_format == INPUT_AUTO) {
        bool any_pbs = false;
        bool any_source = false;
        for (std::size_t i = 0; i < command.filenames.size(); ++i) {
            if (ends_with(command.filenames[i], ".pbs")) any_pbs = true;
            else any_source = true;
        }
        if (any_pbs && any_source) {
            throw std::runtime_error(
                "mixed implicit .pbs and source inputs are not allowed; use --descriptor-set explicitly");
        }
        command.input_format = any_pbs ? INPUT_DESCRIPTOR_SET : INPUT_PROTO_SOURCE;
    }

#if !EASYPB_CODEGEN_WITH_PROTO_PARSER
    if (command.input_format != INPUT_DESCRIPTOR_SET) {
        throw std::runtime_error(
            "this codegen build has no .proto parser; use --descriptor-set file.pbs");
    }
#endif

    return command;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const CommandLine command = parse_cmdline(argc, argv);
        if (command.exit_after_help) return 0;

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
        if (command.action == ACTION_BENCHMARK_PARSER) {
            return easypb_proto::run_parser_benchmark(
                command.filenames, command.benchmark_milliseconds,
                std::cout, std::cerr);
        }
#endif

        for (std::size_t i = 0; i < command.filenames.size(); ++i) {
            const std::string& filename = command.filenames[i];
            std::string contents;
            if (!read_file(filename, contents)) {
                throw std::runtime_error(filename + ": cannot read file");
            }

            if (command.input_format == INPUT_DESCRIPTOR_SET) {
                const FileDescriptorProto file =
                    decode_single_file_descriptor(filename, contents);
#if EASYPB_CODEGEN_WITH_PROTO_PARSER
                if (command.action == ACTION_PRINT_DESCRIPTOR) {
                    if (command.filenames.size() > 1) {
                        std::cout << "== " << filename << " ==\n";
                    }
                    easypb_proto::print_descriptor(file, std::cout);
                    continue;
                }
#endif
                const std::string generated = generator(file);
                std::cout << myformat(FILE_TEMPLATE, filename) << generated;
                continue;
            }

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
            easypb_proto::ParsedProto parsed;
            if (!parse_source_file(filename, contents, parsed)) return 1;
            if (command.action == ACTION_PRINT_DESCRIPTOR) {
                if (command.filenames.size() > 1) {
                    std::cout << "== " << filename << " ==\n";
                }
                easypb_proto::print_descriptor(parsed, std::cout);
                continue;
            }
            if (has_unresolved_types(parsed)) {
                throw std::runtime_error(
                    filename +
                    ": generation stopped because imported type linking is not implemented; "
                    "use protoc and codegen --descriptor-set for schemas with unresolved imports");
            }
            const std::string generated = generator(parsed.file);
            std::cout << myformat(FILE_TEMPLATE, filename) << generated;
#else
            (void)contents;
            throw std::runtime_error(
                "this codegen build has no .proto parser; use --descriptor-set file.pbs");
#endif
        }
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Exception: %s\n", error.what());
        return 1;
    }

    return 0;
}
