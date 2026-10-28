#ifndef EASYPB_CODEGEN_WITH_PROTO_PARSER
#define EASYPB_CODEGEN_WITH_PROTO_PARSER 1
#endif

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "popl.hpp"
#include "descriptor_adapter.hpp"
#include "descriptor_loader.hpp"
#include "file_paths.hpp"
#include "generation.hpp"
#include "generation_options.hpp"
#include "header_output.hpp"
#include "schema_linker.hpp"
#include "schema_semantics.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
// U-36: _isatty/_setmode and _O_BINARY, used to keep redirected stdout LF-only.
#include <fcntl.h>
#include <io.h>
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

// The two helpers below belong to the parser-enabled profile: the descriptor
// printing and the parser benchmark are the only callers, and the profile
// without the parser refuses those modes before they are reached. Outside the
// guard they are dead code, and a dead function is a warning in every build
// of that profile (measured: the lite build of 2026-10-22).
#if EASYPB_CODEGEN_WITH_PROTO_PARSER
bool read_file(const std::string& filename, std::string& contents,
               std::string& error)
{
    // The native read: wide on Windows, so a non-ASCII spelling and a path
    // beyond MAX_PATH stay addressable on the utility modes exactly as they
    // are on the generation path (S4/W-24). The embedded-NUL guard lives
    // there too. The reason is returned instead of being dropped: the print
    // mode used to collapse a denial and a directory into "cannot read file"
    // (CR-37).
    return easypb_file::read_disk_file(filename, contents, error);
}

FileDescriptorSet decode_descriptor_set(const std::string& filename,
                                        const std::string& contents)
{
    // The nesting bound, before the decode: the decode descends into every
    // nested message the set carries, so a set that nests deeper than the
    // limit would overflow the stack inside the runtime's message codec. The
    // check runs on the bytes and stops at the limit, so it cannot overflow
    // the stack either.
    easypb_schema::Diagnostic error;
    if (!easypb_schema::check_descriptor_set_nesting(contents, filename,
                                                     error)) {
        throw std::runtime_error(filename + ": " + error.message);
    }
    // V-11: the decode used to throw the codec's sentence alone, which names
    // no input when several containers are inspected in one call. The
    // generation path already prefixes its container, so the print refusal
    // does the same and keeps the codec's text unchanged.
    FileDescriptorSet set;
    try {
        set = easypb::decode<FileDescriptorSet>(contents);
    } catch (const std::exception& problem) {
        throw std::runtime_error(filename + ": " + problem.what());
    }
    if (set.file.empty()) {
        throw std::runtime_error(filename +
            ": descriptor set contains no FileDescriptorProto");
    }
    // U-61: a set is a container, and inspection prints every file it holds.
    // Refusing a multi-file set here made `--print-descriptor` unable to look
    // at the bundles protoc writes with --include_imports, while the
    // generation path never used this helper.
    //
    // V-18/V-42: the print door reads the same bytes the generation door
    // reads, so it asks the same input rules. The name rule is what makes
    // `--print-descriptor` refuse `../escape.proto` exactly as `--out-dir`
    // does, and the label rule keeps a crafted out-of-range label from being
    // printed as part of a model the generation door would refuse. An unnamed
    // file has no name to validate; its label rule still applies.
    easypb_schema::Diagnostic input_error;
    for (std::size_t i = 0; i < set.file.size(); ++i) {
        if (!set.file[i].name.empty() &&
            !easypb_schema::validate_descriptor_file_name(
                set.file[i].name, filename, input_error)) {
            throw std::runtime_error(filename + ": " + input_error.message);
        }
        if (!easypb_schema::validate_descriptor_field_labels(
                set.file[i], filename, input_error)) {
            throw std::runtime_error(filename + ": " + input_error.message);
        }
    }
    return set;
}
#endif

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
    if (diagnostic.warning) output << "warning: ";
    output << diagnostic.message;
    std::cerr << output.str() << '\n';
}

// The warnings the load and the link stored - a dropped `extend` declaration,
// a compatibility reference into a missing dependency - belong to the run, not
// to the source frontend that happened to produce them. They are printed once,
// on stderr, before generation; the exit code stays 0 and stdout stays the
// header only (S4/W-14). The print path keeps its own per-file loop because it
// never links.
void report_schema_warnings(const easypb_schema::SchemaSet& schema)
{
    const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& files =
        schema.files();
    for (std::size_t i = 0; i < files.size(); ++i) {
        for (std::size_t j = 0; j < files[i]->warnings.size(); ++j) {
            report_schema_diagnostic(files[i]->warnings[j]);
        }
    }
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

// Every successful path ends here: stdout carries the whole result (the
// generated header, the help text, the printed descriptor, the benchmark
// report), so a lost write is a failure, not a success. A closed pipe, a full
// disk or a quota error sets the stream state on the flush; the number of
// bytes accepted is not an observable the stream exposes. Without this check
// `codegen x.proto > x.pb.hpp` left a truncated header and exited 0 on every
// one of those failures. There is no file to name in the diagnostic: the
// stream is the target.
bool flush_stdout()
{
    std::cout.flush();
    if (std::cout) return true;
    report_schema_diagnostic(
        make_schema_diagnostic(std::string(), "cannot write to stdout"));
    return false;
}

#ifdef _WIN32
// U-36: the CRT writes stdout in text mode, so every '\n' becomes CRLF as soon
// as the stream is a file or a pipe, while `--out-dir` publishes the same
// header through the native writer as LF-only bytes. Redirecting stdout is the
// documented way to produce a header, so the two spellings of one output must
// agree byte for byte; the switch is made once, before anything is written.
// An interactive console keeps its text mode, where the translation is the
// console's own line discipline.
void use_lf_stdout()
{
    if (_isatty(_fileno(stdout)) == 0) {
        _setmode(_fileno(stdout), _O_BINARY);
    }
}
#endif

// True for a spelling whose last byte is a path separator. On Windows both
// separators are path syntax; on POSIX a backslash is an ordinary file-name
// byte, so only '/' counts there.
bool ends_with_path_separator(const std::string& path)
{
    if (path.empty()) return false;
    const char last = path[path.size() - 1];
    if (last == '/') return true;
#ifdef _WIN32
    if (last == '\\') return true;
#endif
    return false;
}

// The `.pbs` inference compares the suffix case-insensitively: Windows
// filesystems are case-insensitive, so `SET.PBS` is a natural spelling of a
// descriptor set. Only ASCII letters fold, and a non-ASCII byte can never
// match an ASCII suffix byte. CR-38.
bool has_pbs_suffix(const std::string& name)
{
    static const char suffix[] = ".pbs";
    const std::size_t length = sizeof(suffix) - 1;
    if (name.size() < length) return false;
    const std::size_t offset = name.size() - length;
    for (std::size_t i = 0; i < length; ++i) {
        char actual = name[offset + i];
        if (actual >= 'A' && actual <= 'Z') {
            actual = static_cast<char>(actual - 'A' + 'a');
        }
        if (actual != suffix[i]) return false;
    }
    return true;
}

// V-46: `-I=` writes an empty short-form value, while `-I ""` writes the same
// empty value as the next token. popl records both as one empty value, so the
// spelling is read from the argument list. The walk follows the parser's own
// consumption rules: a first `--` ends option parsing, and the next token
// after an option that requires a value is that option's value whatever it
// looks like (W-22), so `--out-dir -I=` still names a directory.
bool long_option_takes_value(const std::string& argument)
{
    static const char* const names[] = {
        "--proto_path", "--target", "--out-dir", "--benchmark-ms",
        "--string-type", "--repeated-type", "--map-type"
    };
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (argument == names[i]) return true;
    }
    return false;
}

bool short_option_takes_next(const std::string& argument)
{
    if (argument.size() < 2 || argument[0] != '-' || argument[1] == '-') {
        return false;
    }
    for (std::size_t i = 1; i < argument.size(); ++i) {
        // The four short options that require a value. The rest of the token
        // is the value unless the option is its last character; an unknown
        // letter belongs to neither group and consumes nothing.
        if (argument[i] == 'I' || argument[i] == 's' ||
            argument[i] == 'r' || argument[i] == 'm') {
            return i + 1 == argument.size();
        }
    }
    return false;
}

bool short_inline_empty_proto_path(const std::string& argument)
{
    if (argument.size() < 3 || argument[0] != '-' || argument[1] == '-') {
        return false;
    }
    for (std::size_t i = 1; i < argument.size(); ++i) {
        if (argument[i] == 'I' || argument[i] == 's' ||
            argument[i] == 'r' || argument[i] == 'm') {
            // The first value-taking letter owns the rest of the token, so
            // only a leading -I can be the empty search root; `-sI=` names a
            // string type. One '=' is the separator, so a value that is
            // exactly "=" is the empty value; `-I==` keeps its literal '='
            // value.
            return argument[i] == 'I' &&
                   argument.compare(i + 1, std::string::npos, "=") == 0;
        }
    }
    return false;
}

bool has_empty_short_proto_path(const std::vector<std::string>& arguments)
{
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i] == "--") return false;
        if (short_inline_empty_proto_path(arguments[i])) return true;
        if (long_option_takes_value(arguments[i]) ||
            short_option_takes_next(arguments[i])) {
            ++i;  // the next token is this option's value
        }
    }
    return false;
}

CommandLine parse_cmdline(const std::vector<std::string>& arguments)
{
    using namespace popl;

#if !EASYPB_CODEGEN_WITH_PROTO_PARSER
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--") break;
        if (argument == "--print-descriptor" ||
            argument == "--benchmark-parser" ||
            argument == "--benchmark-ms" ||
            argument.find("--benchmark-ms=") == 0) {
            throw std::runtime_error(
                argument + " is unavailable because this codegen build has no .proto parser");
        }
    }
#endif

    // The prescan that used to reject `--name=` here is gone: the option
    // parser distinguishes an inline empty value from an absent one and its
    // Value<std::string>/Value<T> refuse the empty string. The parser's own
    // failure is translated below into the sentence this tool documents, so
    // the grammar stays in one place and the value position is not inspected
    // (W-22, W-23).

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
    auto proto_path_option = parser.add<EmptyValue<std::string> >(
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
    try {
        parser.parse(static_cast<int>(arguments.size()),
                     argv_pointers.empty() ? static_cast<const char* const*>(0)
                                           : &argv_pointers[0]);
    } catch (const popl::invalid_option& problem) {
        // Value<std::string>::parse and Value<T>::parse refuse an empty value
        // with this error and the spelling the user typed. An option the
        // parser itself reports as missing a value ("Missing value for flag:
        // -s") has no name here, so it keeps its own sentence.
        if (problem.error() == popl::invalid_option::Error::missing_argument &&
            problem.option() != 0 &&
            problem.what_name() != popl::OptionName::unspecified) {
            throw std::runtime_error(
                problem.option()->name(problem.what_name(), true) +
                " requires a non-empty value");
        }
        throw;
    }

    // V-23: `--help` is the first dispatch, so it wins over the two output
    // modes as it already wins over an unknown flag (owner decision U-45).
    // `--groff`/`--bash` shape the help text only when help was not asked for.
    if (help_option->count() || arguments.size() <= 1) {
        std::cout << usage_text() << '\n' << parser << '\n';
        command.exit_after_help = true;
        return command;
    }
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

    // An unknown or misspelled option is refused instead of silently ignored.
    // The parser records it and never applies it, so a typo such as
    // "--no-has-field" for "--no-has-fields" would otherwise generate a header
    // that keeps the feature the caller asked to drop. Help wins over the
    // unknown list: the owner decision for review 1027 U-45 (variant A) makes
    // the presence of `--help` produce help and exit 0 whatever else the
    // command line carries, so a caller who asks for help is never answered
    // with a typo report. A token after "--" is positional by definition and
    // never reaches this list.
    if (!parser.unknown_options().empty()) {
        throw std::runtime_error("Unknown flag: " +
                                 parser.unknown_options()[0]);
    }

    // V-46: `-I=` is the one short inline empty spelling the reference
    // compiler refuses ("--proto_path passed empty directory name.  (Use \".\"
    // for current directory.)", measured on libprotoc 30.1). The next-token
    // and long inline spellings keep the working-directory meaning below.
    if (has_empty_short_proto_path(arguments)) {
        throw std::runtime_error(
            "-I=: --proto_path passed empty directory name.  (Use \".\" for "
            "current directory.)");
    }

    // A truly empty argv is help; options that name no input are an error.
    // The reference compiler keeps the two apart: it prints help and exits 0
    // only when no argument at all was given (command_line_interface.cc:
    // 1806-1810) and answers "Missing input file." with a nonzero exit when a
    // mode was selected but no input file named (1884-1888). Keying help on
    // the positional list alone hid an empty file list behind a successful
    // run, so a build script whose list expanded to nothing reported success
    // and generated nothing. The sentence is the reference one; the shared
    // refusal path in main prints it verbatim, without a generic prefix.
    if (parser.non_option_args().empty()) {
        throw std::runtime_error("Missing input file.");
    }

    // A positional input named twice is compiled once. The loader keys files
    // by logical name, so generation has always accepted `a.proto a.proto`
    // silently; keeping that convenience is the owner decision for review
    // 1027 U-19 (variant B). Deduplicating at the command line gives the
    // utility modes, the mixed-input check and the per-file print headings
    // the same one-input list, so `--print-descriptor a.proto a.proto` is
    // byte-identical to the single-input run. protoc refuses a repeated
    // input whose output it would write twice ("Tried to write the same file
    // twice"), so the silence stays a documented divergence.
    command.filenames.clear();
    for (std::size_t i = 0; i < parser.non_option_args().size(); ++i) {
        const std::string& filename = parser.non_option_args()[i];
        bool seen = false;
        for (std::size_t j = 0; j < command.filenames.size(); ++j) {
            if (command.filenames[j] == filename) {
                seen = true;
                break;
            }
        }
        if (!seen) command.filenames.push_back(filename);
    }
    // A trailing separator is never part of a file name the OS resolves as
    // that file: `simple.proto/` names a directory entry that does not exist.
    // `lexical_absolute` used to trim the separator, so the status query found
    // the regular file and the run accepted a spelling the OS does not resolve
    // as it (CR-41). Refuse the spelling as typed, before any mode reads it.
    for (std::size_t i = 0; i < command.filenames.size(); ++i) {
        if (ends_with_path_separator(command.filenames[i])) {
            throw std::runtime_error(command.filenames[i] +
                ": input file name must not end with a path separator");
        }
    }
    if (descriptor_set_option->is_set()) {
        command.input_format = INPUT_DESCRIPTOR_SET;
    }

    for (std::size_t i = 0; i < proto_path_option->count(); ++i) {
        // An explicitly empty value names the working directory when it is
        // written as the next token or as the long inline form, exactly as
        // the reference compiler's --proto_path= does; the loader normalizes
        // it (load_source_files: an empty root entry is "."). The short inline
        // spelling `-I=` is refused above, and every other string option still
        // refuses an empty value in the parser.
        command.proto_paths.push_back(proto_path_option->value(i));
    }
    for (std::size_t i = 0; i < target_option->count(); ++i) {
        // An empty value cannot reach this loop: every Value<std::string>
        // refuses it in the parser, and the translation above turns that into
        // the tool's sentence. The one exception is -I/--proto_path, whose
        // EmptyValue keeps the empty spelling because it means the working
        // directory; that option is read by the proto-path loop above.
        const std::string target = target_option->value(i);
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

    const bool generation_option_set =
        no_class_option->is_set() || no_decoder_option->is_set() ||
        no_encoder_option->is_set() || no_has_option->is_set() ||
        no_required_option->is_set() || no_defaults_option->is_set() ||
        packed_option->is_set() || no_packed_option->is_set() ||
        allow_self_recursive_containers_option->is_set() ||
        string_type_option->is_set() || repeated_type_option->is_set() ||
        map_type_option->is_set() ||
        target_option->is_set() || out_dir_option->is_set();
    if (command.action != ACTION_GENERATE && proto_path_option->is_set()) {
        // -I/--proto_path is a source-generation search root, not a
        // code-generation option: the utility modes parse each file on its
        // own and never resolve imports, so a root has nothing to search.
        // Naming it as a generation option sent the reader to the wrong
        // documentation (CR-39).
        throw std::runtime_error(
            "-I/--proto_path is a search-root option for .proto generation; descriptor print and parser benchmark modes do not resolve imports");
    }
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
            if (has_pbs_suffix(command.filenames[i])) any_pbs = true;
            else any_source = true;
        }
        if (any_pbs && any_source) {
            throw std::runtime_error(
                "mixed implicit .pbs and source inputs are not allowed; use --descriptor-set explicitly");
        }
        command.input_format = any_pbs ? INPUT_DESCRIPTOR_SET : INPUT_PROTO_SOURCE;
    }

    // U-35: the benchmark parses source text, so descriptor-set input is
    // refused on BOTH spellings - the explicit switch and a `.pbs` positional
    // the inference just classified. The check used to sit beside the mode
    // table, before inference, so `--benchmark-parser x.pbs` decoded the bytes
    // as source and reported a successful benchmark.
    if (command.action == ACTION_BENCHMARK_PARSER &&
        command.input_format == INPUT_DESCRIPTOR_SET) {
        throw std::runtime_error(
            "--benchmark-parser accepts .proto source files, not descriptor-set input");
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
// V-25: the print mode used to show whatever the parser or decoder built, so a
// MessageSet shape the generation path refuses printed and exited 0. The rule
// is reference-free - the option and the field list are in the descriptor
// alone - so the same pass `link_schema` runs answers the print mode without
// linking, and the refusal is the generation path's own sentence.
bool validate_printed_message_set(easypb_schema::SchemaFile& file)
{
    easypb_schema::Diagnostic error;
    if (easypb_schema::validate_message_set_wire_format(file, error)) {
        return true;
    }
    report_schema_diagnostic(error);
    return false;
}

// Descriptor printing keeps its standalone path: no linking, no import
// resolution and no reading of the new generation-only options (they are
// refused before this runs). The one semantic question it asks is the
// reference-free MessageSet rule (V-25). The function exists only in the
// parser-enabled profile, like the action that reaches it.
//
// U-34: the dump is assembled in memory and reaches stdout only after every
// input has been read, decoded and parsed. The old loop wrote each tree as it
// went, so a failure on the second input left a truncated dump on stdout that
// the caller could not tell from a complete one. The benchmark lane already
// read every file first; this gives the print lane the same all-or-nothing
// contract.
//
// U-61: a descriptor set is a container and every file it holds is printed.
// Inspection is not generation, so there is no single root to select.
int run_print_descriptor(const CommandLine& command)
{
    std::ostringstream dump;
    for (std::size_t i = 0; i < command.filenames.size(); ++i) {
        const std::string& filename = command.filenames[i];
        std::string contents;
        std::string read_error;
        if (!read_file(filename, contents, read_error)) {
            throw std::runtime_error(filename + ": " + read_error);
        }

        if (command.input_format == INPUT_DESCRIPTOR_SET) {
            const FileDescriptorSet set =
                decode_descriptor_set(filename, contents);
            if (command.filenames.size() > 1) {
                dump << "== " << filename << " ==\n";
            }
            for (std::size_t j = 0; j < set.file.size(); ++j) {
                // V-25: the decoded tree borrows its strings from `contents`,
                // which lives for the whole loop; the view therefore carries
                // exactly the label and option the generation path validates.
                easypb_schema::SchemaFile view;
                view.file = set.file[j];
                view.physical_name = filename;
                view.from_descriptor_set = true;
                if (!validate_printed_message_set(view)) return 1;
                easypb_proto::print_descriptor(set.file[j], dump);
            }
            continue;
        }

        easypb_proto::ParsedProto parsed;
        if (!parse_source_file(filename, contents, parsed)) return 1;
        if (!validate_printed_message_set(parsed)) return 1;
        if (command.filenames.size() > 1) {
            dump << "== " << filename << " ==\n";
        }
        easypb_proto::print_descriptor(parsed, dump);
    }
    std::cout << dump.str();
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

    // The load and the link stored every warning the schema produced. The
    // generation path used to drop them, although OPTIONS.md promises the
    // compatibility warning; print them once, before generation, with the
    // exit code still 0 and stdout still the header only (S4/W-14).
    report_schema_warnings(schema);

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
        // Generate into memory first: a failure must leave stdout empty. The
        // failure is a diagnostic, not an exception text: the --out-dir path
        // builds the same one (generate_files), so both print one line in one
        // shape, naming the target (W-66) and, for a declaration failure, its
        // source position (U-30).
        std::string header;
        try {
            header = easypb_codegen::generate_header(*targets[0], linked,
                                                     command.generation_options);
        } catch (const easypb_codegen::EmitterError& problem) {
            easypb_schema::Diagnostic diagnostic = make_schema_diagnostic(
                easypb_schema::schema_file_label(*targets[0]),
                problem.what());
            diagnostic.location =
                easypb_codegen::emitter_declaration_location(
                    *targets[0], problem.declaration_path());
            report_schema_diagnostic(diagnostic);
            return 1;
        } catch (const std::exception& problem) {
            report_schema_diagnostic(make_schema_diagnostic(
                easypb_schema::schema_file_label(*targets[0]),
                problem.what()));
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
    use_lf_stdout();
#else
    // A write to a closed pipe raises SIGPIPE, whose default action kills the
    // process before the stream state can report the failure. The contract is a
    // reported failure with a nonzero exit (P5), so the signal is ignored here
    // and the flush check below turns the failed write into the diagnostic.
    // Windows reports ERROR_BROKEN_PIPE from the write instead.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        std::vector<std::string> arguments;
        if (!collect_arguments(argc, argv, arguments)) {
            std::fprintf(stderr,
                         "command line is not valid UTF-8\n");
            return 1;
        }
        const CommandLine command = parse_cmdline(arguments);
        if (command.exit_after_help) return flush_stdout() ? 0 : 1;

        int status = 0;
#if EASYPB_CODEGEN_WITH_PROTO_PARSER
        if (command.action == ACTION_BENCHMARK_PARSER) {
            status = easypb_proto::run_parser_benchmark(
                command.filenames, command.benchmark_milliseconds,
                std::cout, std::cerr);
        } else if (command.action == ACTION_PRINT_DESCRIPTOR) {
            status = run_print_descriptor(command);
        } else
#endif
        {
            status = run_generation(command);
        }
        // A failure has already reported itself; a success still has to prove
        // that its stdout bytes reached the caller (CR-14).
        if (status != 0) return status;
        return flush_stdout() ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "unknown error\n");
        return 1;
    }

    return 0;
}
