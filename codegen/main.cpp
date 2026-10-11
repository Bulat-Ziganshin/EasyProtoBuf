#ifndef EASYPB_CODEGEN_WITH_PROTO_PARSER
#define EASYPB_CODEGEN_WITH_PROTO_PARSER 1
#endif

#include <cstddef>
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
#include "descriptor_loader.hpp"
#include "file_paths.hpp"
#include "generation.hpp"
#include "generation_options.hpp"
#include "header_output.hpp"
#include "schema_linker.hpp"
#include "utils.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
#include "parser/parser_benchmark.hpp"
#include "parser/pretty_printer.hpp"
#include "parser/proto_parser.hpp"
#include "proto_loader.hpp"
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
    std::vector<std::string> proto_paths;   // ordered, repeatable -I/--proto_path
    std::vector<std::string> targets;       // ordered, deduplicated --target
    std::string out_dir;
    bool has_out_dir;
    easypb_codegen::GenerationOptions generation_options;
    bool exit_after_help;

    CommandLine()
        : action(ACTION_GENERATE), input_format(INPUT_AUTO),
          benchmark_milliseconds(100), has_out_dir(false),
          exit_after_help(false) {}
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

// The process boundary is the ONE place arguments are converted. On Windows
// the CRT `argv` is decoded through the active code page, which loses any
// non-ASCII path before a loader could see it; the wide command line is
// converted here, once, to the UTF-8 spelling the rest of Codegen uses
// (plan 5 C4). On POSIX the argument bytes pass through unchanged.
bool collect_arguments(int argc, char** argv, std::vector<std::string>& arguments)
{
    arguments.clear();
#ifdef _WIN32
    int wide_count = 0;
    LPWSTR* wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (wide_arguments == 0) return false;
    arguments.reserve(static_cast<std::size_t>(wide_count));
    for (int i = 0; i < wide_count; ++i) {
        std::string text;
        if (!easypb_file::utf8_from_wide(wide_arguments[i], text)) {
            LocalFree(wide_arguments);
            return false;
        }
        arguments.push_back(text);
    }
    LocalFree(wide_arguments);
    (void)argc;
    (void)argv;
    return true;
#else
    (void)argc;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.push_back(argv[i]);
    return true;
#endif
}

bool read_file(const std::string& filename, std::string& contents)
{
    // Native file calls truncate at an embedded NUL while the spelling
    // keeps its full length: refuse instead of reading a prefix.
    if (filename.find('\0') != std::string::npos) return false;
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

// One place that renders a shared Diagnostic, so load, link, generation and
// publication failures read the same way and all of them land on stderr.
void report_schema_diagnostic(const easypb_schema::Diagnostic& diagnostic)
{
    std::ostringstream output;
    if (!diagnostic.file.empty()) output << diagnostic.file;
    if (diagnostic.location.line != 0) {
        if (!diagnostic.file.empty()) output << ':';
        output << diagnostic.location.line << ':'
               << diagnostic.location.column;
    }
    if (output.tellp() > 0) output << ": ";
    output << diagnostic.message;
    std::cerr << output.str() << '\n';
}

easypb_schema::Diagnostic make_schema_diagnostic(const std::string& file,
                                                 const std::string& message)
{
    easypb_schema::Diagnostic diagnostic;
    diagnostic.file = file;
    diagnostic.location = easypb_schema::unknown_location();
    diagnostic.message = message;
    diagnostic.code = easypb_schema::DIAGNOSTIC_GENERIC;
    return diagnostic;
}

CommandLine parse_cmdline(const std::vector<std::string>& arguments)
{
    using namespace popl;

#if !EASYPB_CODEGEN_WITH_PROTO_PARSER
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--print-descriptor" ||
            argument == "--benchmark-parser" ||
            argument == "--benchmark-ms" ||
            argument.find("--benchmark-ms=") == 0) {
            throw std::runtime_error(
                argument + " is unavailable because this codegen build has no .proto parser");
        }
    }
#endif

    // A long option spelled `--name=` hands the parser an empty value, and
    // the option parser would consume the NEXT argument as that value. Refuse
    // the spelling before any argument is interpreted (plan 5 C1: empty
    // values are rejected).
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--") break;
        if (argument.size() > 2 && argument[0] == '-' && argument[1] == '-') {
            const std::size_t equals = argument.find('=');
            if (equals != std::string::npos && equals + 1 == argument.size()) {
                throw std::runtime_error(
                    argument.substr(0, equals) +
                    " requires a non-empty value");
            }
        }
    }

    CommandLine command;
    OptionParser parser("Allowed options");

    auto help_option  = parser.add<Switch>("h", "help", "produce help message");
    auto groff_option = parser.add<Switch>("", "groff", "produce groff formatted help message");
    auto bash_option  = parser.add<Switch>("", "bash", "produce bash completion script");
    auto descriptor_set_option = parser.add<Switch>(
        "", "descriptor-set", "read binary FileDescriptorSet input");

    // Plan 5's generation-only options. Both spellings of a repeated value
    // are collected in argv order through count()/value(index); the option
    // parser does not collapse them, and the CLI never keeps only the last
    // occurrence.
    auto proto_path_option = parser.add<Value<std::string> >(
        "I", "proto_path",
        "add a .proto search root (ordered, repeatable; empty list means the current directory)");
    auto target_option = parser.add<Value<std::string> >(
        "", "target",
        "logical .proto name to generate from a descriptor set (repeatable)");
    auto out_dir_option = parser.add<Value<std::string> >(
        "", "out-dir",
        "write one generated header per selected target below this directory");

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
        "c", "no-class", "don't generate C++ struct", &command.generation_options.no_class);
    auto no_decoder_option = parser.add<Switch>(
        "d", "no-decoder", "don't generate decoder", &command.generation_options.no_decoder);
    auto no_encoder_option = parser.add<Switch>(
        "e", "no-encoder", "don't generate encoder", &command.generation_options.no_encoder);
    auto no_has_option = parser.add<Switch>(
        "f", "no-has-fields", "don't generate has_* fields", &command.generation_options.no_has_fields);
    auto no_required_option = parser.add<Switch>(
        "", "no-required", "ignore 'required' attribute", &command.generation_options.no_required);
    auto no_defaults_option = parser.add<Switch>(
        "", "no-default-values", "ignore default values", &command.generation_options.no_default_values);
    auto packed_option = parser.add<Switch>(
        "p", "packed", "make all repeated fields packed when allowed", &command.generation_options.packed);
    auto no_packed_option = parser.add<Switch>(
        "", "no-packed", "make all repeated fields non-packed", &command.generation_options.no_packed);
    auto allow_self_recursive_containers_option = parser.add<Switch>(
        "", "allow-self-recursive-containers",
        "allow direct self-recursive repeated/map message fields",
        &command.generation_options.allow_self_recursive_containers);

    auto string_type_option = parser.add<Value<std::string> >(
        "s", "string-type", "C++ type for string/bytes fields",
        "::std::string", &command.generation_options.cpp_string_type);
    auto repeated_type_option = parser.add<Value<std::string> >(
        "r", "repeated-type", "C++ container type for repeated fields",
        "::std::vector", &command.generation_options.cpp_repeated_type);
    auto map_type_option = parser.add<Value<std::string> >(
        "m", "map-type", "C++ container type for map fields",
        "::std::map", &command.generation_options.cpp_map_type);

    std::vector<const char*> argv_pointers;
    argv_pointers.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        argv_pointers.push_back(arguments[i].c_str());
    }
    parser.parse(static_cast<int>(arguments.size()),
                 argv_pointers.empty() ? static_cast<const char* const*>(0)
                                       : &argv_pointers[0]);

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

    for (std::size_t i = 0; i < proto_path_option->count(); ++i) {
        command.proto_paths.push_back(proto_path_option->value(i));
    }
    for (std::size_t i = 0; i < target_option->count(); ++i) {
        const std::string target = target_option->value(i);
        if (target.empty()) {
            throw std::runtime_error("--target requires a non-empty name");
        }
        bool seen = false;
        for (std::size_t j = 0; j < command.targets.size(); ++j) {
            if (command.targets[j] == target) {
                seen = true;
                break;
            }
        }
        if (!seen) command.targets.push_back(target);
    }
    if (out_dir_option->count() > 1) {
        throw std::runtime_error("--out-dir may be given only once");
    }
    if (out_dir_option->count() == 1) {
        command.out_dir = out_dir_option->value(0);
        command.has_out_dir = true;
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
        map_type_option->is_set() ||
        proto_path_option->is_set() || target_option->is_set() ||
        out_dir_option->is_set();
    if (command.action != ACTION_GENERATE && generation_option_set) {
        throw std::runtime_error(
            "code-generation options cannot be used with descriptor print or parser benchmark modes");
    }
#endif

    // Derived behavior and validation live with the options, so a core-only
    // caller gets the same normalization as the CLI (plan 5 S3/S4).
    std::string options_error;
    if (!easypb_codegen::finalize_generation_options(
            command.generation_options, options_error)) {
        throw std::runtime_error(options_error);
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

    // The mode table's option halves: -I addresses source search roots and
    // --target addresses files inside descriptor containers. Each is refused
    // in the other mode instead of being silently ignored.
    if (command.input_format == INPUT_DESCRIPTOR_SET) {
        if (!command.proto_paths.empty()) {
            throw std::runtime_error(
                "-I/--proto_path is valid only with .proto source input");
        }
    } else if (!command.targets.empty()) {
        throw std::runtime_error(
            "--target is valid only with --descriptor-set input");
    }

#if !EASYPB_CODEGEN_WITH_PROTO_PARSER
    if (command.input_format != INPUT_DESCRIPTOR_SET) {
        throw std::runtime_error(
            "this codegen build has no .proto parser; use --descriptor-set file.pbs");
    }
#endif

    return command;
}

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
// Descriptor printing keeps its standalone, permissive path: one descriptor
// file per container, no linking, and no reading of the new generation-only
// options (they are refused before this runs). The function exists only in
// the parser-enabled profile, like the action that reaches it.
int run_print_descriptor(const CommandLine& command)
{
    for (std::size_t i = 0; i < command.filenames.size(); ++i) {
        const std::string& filename = command.filenames[i];
        std::string contents;
        if (!read_file(filename, contents)) {
            throw std::runtime_error(filename + ": cannot read file");
        }

        if (command.input_format == INPUT_DESCRIPTOR_SET) {
            const FileDescriptorProto file =
                decode_single_file_descriptor(filename, contents);
            if (command.filenames.size() > 1) {
                std::cout << "== " << filename << " ==\n";
            }
            easypb_proto::print_descriptor(file, std::cout);
            continue;
        }

        easypb_proto::ParsedProto parsed;
        if (!parse_source_file(filename, contents, parsed)) return 1;
        if (command.filenames.size() > 1) {
            std::cout << "== " << filename << " ==\n";
        }
        easypb_proto::print_descriptor(parsed, std::cout);
    }
    return 0;
}
#endif

// Load everything, select the targets the mode table names, link once, and
// then either stream the single selected header to stdout or publish all of
// them below --out-dir. Nothing reaches stdout before every target has been
// generated and validated (plan 5 C5, P1).
int run_generation(const CommandLine& command)
{
    easypb_schema::SchemaSet schema;
    easypb_schema::Diagnostic error;
    easypb_schema::LinkMode mode = easypb_schema::LINK_COMPLETE;

    if (command.input_format == INPUT_DESCRIPTOR_SET) {
        if (!easypb_schema::load_descriptor_sets(command.filenames, schema,
                                                 error)) {
            report_schema_diagnostic(error);
            return 1;
        }
        if (command.targets.empty()) {
            // One loaded file keeps the thin-descriptor compatibility path;
            // several files cannot pick a root from graph order and ask for
            // an explicit selection instead.
            if (schema.files().size() != 1) {
                std::ostringstream message;
                message << "loaded " << schema.files().size()
                        << " descriptor files; --target NAME is required to "
                           "select which ones to generate";
                report_schema_diagnostic(
                    make_schema_diagnostic(std::string(), message.str()));
                return 1;
            }
            schema.add_target(*schema.files()[0]);
            mode = easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY;
        } else {
            for (std::size_t i = 0; i < command.targets.size(); ++i) {
                easypb_schema::SchemaFile* file =
                    schema.find_file(command.targets[i]);
                if (file == 0) {
                    report_schema_diagnostic(make_schema_diagnostic(
                        command.targets[i],
                        "target \"" + command.targets[i] +
                            "\" is not among the loaded descriptor files"));
                    return 1;
                }
                schema.add_target(*file);
            }
        }
    } else {
#if EASYPB_CODEGEN_WITH_PROTO_PARSER
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths = command.proto_paths;
        if (!easypb_proto::load_source_files(options, command.filenames,
                                             schema, error)) {
            report_schema_diagnostic(error);
            return 1;
        }
#else
        report_schema_diagnostic(make_schema_diagnostic(
            std::string(),
            "this codegen build has no .proto parser; use --descriptor-set file.pbs"));
        return 1;
#endif
    }

    easypb_schema::LinkedSchema linked;
    if (!easypb_schema::link_schema(schema, mode, linked, error)) {
        report_schema_diagnostic(error);
        return 1;
    }

    const std::vector<easypb_schema::SchemaFile*>& targets = schema.targets();
    if (targets.empty()) {
        report_schema_diagnostic(
            make_schema_diagnostic(std::string(),
                                   "no generation targets were selected"));
        return 1;
    }

    if (!command.has_out_dir) {
        if (targets.size() != 1) {
            std::ostringstream message;
            message << targets.size()
                    << " targets were selected; stdout carries at most one "
                       "generated header, so use --out-dir DIR";
            report_schema_diagnostic(
                make_schema_diagnostic(std::string(), message.str()));
            return 1;
        }
        // Generate into memory first: a failure must leave stdout empty.
        std::string header;
        try {
            header = easypb_codegen::generate_header(*targets[0], linked,
                                                     command.generation_options);
        } catch (const std::exception& problem) {
            std::fprintf(stderr, "Exception: %s\n", problem.what());
            return 1;
        }
        std::cout << header;
        return 0;
    }

    std::vector<easypb_codegen::GeneratedFile> generated;
    if (!easypb_codegen::generate_files(linked, command.generation_options,
                                        generated, error)) {
        report_schema_diagnostic(error);
        return 1;
    }
    if (!easypb_codegen::publish_headers(command.out_dir, generated, error)) {
        report_schema_diagnostic(error);
        return 1;
    }
    return 0;
}

} // namespace

#ifdef _WIN32
// Switches console output to UTF-8 so diagnostic file names and
// generated output (both UTF-8) render correctly, restoring the
// previous page when the guard dies. The destructor runs on every
// return and on every C++ exception unwinding through main, including
// the catch(...) below; only abnormal termination (abort, _exit, kill,
// crash) can skip it, exactly like a manual chcp left behind. When
// output is redirected the switch is harmless: the bytes stay UTF-8.
class ConsoleUtf8Output {
public:
    ConsoleUtf8Output() : previous_(GetConsoleOutputCP()) {
        SetConsoleOutputCP(CP_UTF8);
    }
    ~ConsoleUtf8Output() {
        if (previous_ != 0) SetConsoleOutputCP(previous_);
    }

private:
    UINT previous_;

    ConsoleUtf8Output(const ConsoleUtf8Output&);
    ConsoleUtf8Output& operator=(const ConsoleUtf8Output&);
};
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    const ConsoleUtf8Output console_utf8;
#endif
    try {
        std::vector<std::string> arguments;
        if (!collect_arguments(argc, argv, arguments)) {
            std::fprintf(stderr,
                         "Exception: command line is not valid UTF-8\n");
            return 1;
        }
        const CommandLine command = parse_cmdline(arguments);
        if (command.exit_after_help) return 0;

#if EASYPB_CODEGEN_WITH_PROTO_PARSER
        if (command.action == ACTION_BENCHMARK_PARSER) {
            return easypb_proto::run_parser_benchmark(
                command.filenames, command.benchmark_milliseconds,
                std::cout, std::cerr);
        }
        if (command.action == ACTION_PRINT_DESCRIPTOR) {
            return run_print_descriptor(command);
        }
#endif

        return run_generation(command);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Exception: %s\n", error.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "Exception: unknown error\n");
        return 1;
    }

    return 0;
}
