// Plan 5 follow-up S6: the library API lane.
//
// Rows W-11, W-47, W-48 and W-58 are the shapes a caller that links
// easypb_codegen_core can reach without the CLI: default-constructed
// GenerationOptions, hand-built descriptors whose presence bits disagree with
// their strings, an in-memory compatibility link, and the link result's flag.
// The lane links only easypb_codegen_core (which brings easypb_schema), so it
// is parser-free and runs in the lite profile.
//
// Two halves under one CTest entry:
//
//   1. this binary - the C++ API suite. It also has an
//      `--emit-options-default <path>` mode: the build's compile check runs
//      that mode to produce the W-11 header and compiles it, because cl.exe
//      started from a test script outside the Visual Studio environment
//      cannot find its own standard library headers;
//   2. test_library.cmake - the CLI half: the W-47 refusals and controls, the
//      W-47 source-path control, and the library-vs-CLI byte identity. A
//      library caller must get the CLI's answer, not a parallel one.
//
// The fixtures are committed `.pbs` files rebuilt by data/make_descriptors.py;
// the lane never calls protoc.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <easypb.hpp>

#include "descriptor.pb.hpp"
#include "descriptor_adapter.hpp"
#include "generation.hpp"
#include "generation_options.hpp"
#include "schema.hpp"
#include "schema_linker.hpp"
#include "utils.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression
                  << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

bool read_binary_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool write_binary_file(const std::string& path, const std::string& contents)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) return false;
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    return output.good();
}

bool load_descriptor_set(const std::string& path, easypb_schema::SchemaSet& set,
                         easypb_schema::Diagnostic& error)
{
    std::string bytes;
    if (!read_binary_file(path, bytes)) {
        error.message = "cannot read fixture " + path;
        error.code = easypb_schema::DIAGNOSTIC_GENERIC;
        return false;
    }
    const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(bytes);
    // The decoded views point into `bytes`, which dies at the end of this
    // call. append_descriptor_files deep-copies and interns every retained
    // string, which is exactly the contract this lane relies on.
    return easypb_schema::append_descriptor_files(decoded.file, path, set,
                                                  error);
}

// One named descriptor through the shared adapter, selected as the target.
bool load_single(const std::string& path, easypb_schema::SchemaSet& set,
                 easypb_schema::LinkedSchema& linked,
                 easypb_schema::Diagnostic& error)
{
    if (!load_descriptor_set(path, set, error)) return false;
    if (set.files().size() != 1) {
        error.message = "fixture " + path + " is not a single-file set";
        return false;
    }
    set.add_target(*set.files()[0]);
    return easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                      error);
}

FieldDescriptorProto int32_field(const char* name, std::int32_t number)
{
    FieldDescriptorProto field;
    field.name = name;
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_INT32;
    field.has_type = true;
    return field;
}

FieldDescriptorProto named_field(const char* name, std::int32_t number,
                                 std::int32_t type, const char* type_name)
{
    FieldDescriptorProto field;
    field.name = name;
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = type;
    field.has_type = true;
    field.type_name = type_name;
    field.has_type_name = true;
    return field;
}

void append_message(FileDescriptorProto& file, DescriptorProto message)
{
    file.message_type.push_back(message);
}

DescriptorProto message_with_name(const char* name)
{
    DescriptorProto message;
    message.name = name;
    message.has_name = true;
    return message;
}

// A valid one-file schema: the target every hand-built link case starts from.
void make_simple_file(FileDescriptorProto& file, const char* name,
                      const char* package)
{
    file.name = name;
    file.has_name = true;
    file.package = package;
    file.has_package = true;
    file.syntax = "proto2";
    file.has_syntax = true;
    DescriptorProto message = message_with_name("M");
    message.field.push_back(int32_field("a", 1));
    append_message(file, message);
}

// --- W-11: the entry points finalize a copy ---------------------------------

void test_w11(const std::string& data_dir)
{
    const std::string path = data_dir + "/options-default.pbs";
    easypb_schema::SchemaSet set;
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(load_single(path, set, linked, error));
    if (linked.files == 0) return;

    easypb_codegen::GenerationOptions options;
    std::string header;
    bool threw = false;
    try {
        header = easypb_codegen::generate_header(*set.files()[0], linked,
                                                 options);
    } catch (const std::exception& problem) {
        threw = true;
        std::cerr << "L1: generate_header threw: " << problem.what() << '\n';
    }
    CHECK(!threw);
    CHECK(contains(header, "::std::vector< ::int32_t> a;"));
    CHECK(contains(header, "::std::map< ::int32_t,::std::string> m;"));
    CHECK(!contains(header, "::std::vector a;"));
    CHECK(!contains(header, "::std::map m;"));

    // L2: the same text through generate_files, one entry, no caller setup.
    std::vector<easypb_codegen::GeneratedFile> files;
    CHECK(easypb_codegen::generate_files(linked, options, files, error));
    CHECK(files.size() == 1);
    if (files.size() == 1) {
        CHECK(files[0].name == "options-default.pb.hpp");
        CHECK(files[0].content == header);
    }

    // L4: the contradictory pair is refused with the CLI's sentence, on both
    // entry points.
    easypb_codegen::GenerationOptions contradictory;
    contradictory.packed = true;
    contradictory.no_packed = true;
    std::vector<easypb_codegen::GeneratedFile> refused;
    CHECK(!easypb_codegen::generate_files(linked, contradictory, refused,
                                          error));
    CHECK(refused.empty());
    CHECK(error.message ==
          "Options --packed and --no-packed can't be used together");
    CHECK(error.file.empty());
    CHECK(error.location.line == 0 && error.location.column == 0);

    threw = false;
    std::string thrown;
    try {
        header = easypb_codegen::generate_header(*set.files()[0], linked,
                                                 contradictory);
    } catch (const std::exception& problem) {
        threw = true;
        thrown = problem.what();
    }
    CHECK(threw);
    CHECK(thrown == "Options --packed and --no-packed can't be used together");

    // L5: neither call mutated the caller's object. A caller that wants the
    // normalized values calls finalize_generation_options itself.
    CHECK(contradictory.cpp_repeated_type == "::std::vector");
    CHECK(contradictory.cpp_map_type == "::std::map");
    CHECK(options.cpp_repeated_type == "::std::vector");
    CHECK(options.cpp_map_type == "::std::map");

    // W-50: finalize_generation_options refuses the contradiction BEFORE it
    // normalizes anything, so a caller that ignores the false return reads the
    // object it passed, not a half-normalized mix.
    {
        easypb_codegen::GenerationOptions unnormalized;
        unnormalized.packed = true;
        unnormalized.no_packed = true;
        unnormalized.no_has_fields = true;
        std::string option_error;
        CHECK(!easypb_codegen::finalize_generation_options(unnormalized,
                                                           option_error));
        CHECK(option_error ==
              "Options --packed and --no-packed can't be used together");
        CHECK(!unnormalized.no_required);
        CHECK(unnormalized.cpp_repeated_type == "::std::vector");
        CHECK(unnormalized.cpp_map_type == "::std::map");

        // Control: the success path still normalizes in place.
        easypb_codegen::GenerationOptions normalized;
        CHECK(easypb_codegen::finalize_generation_options(normalized,
                                                          option_error));
        CHECK(normalized.cpp_repeated_type == "::std::vector<{}>");
        CHECK(normalized.cpp_map_type == "::std::map<{0},{1}>");
    }

    // W-35: an empty view may carry a null data(), and myformat must not call
    // append(nullptr, 0).
    CHECK(easypb_codegen::myformat("[{0}]", str_view()) == "[]");

    // W-55: the helpers are owned by easypb_codegen, not by the global
    // namespace the public include directory used to publish.
    CHECK(easypb_codegen::ends_with("a.pbs", ".pbs"));
    CHECK(!easypb_codegen::ends_with("a.proto", ".pbs"));
    CHECK(easypb_codegen::myformat("[{0}]", "x") == "[x]");
}

// --- W-48: the presence bits are normalized at ingest -----------------------

void test_w48_name_bit()
{
    // L12: has_name false on the importer; the duplicate-import diagnostic
    // still carries the file's logical name.
    FileDescriptorProto importer;
    importer.name = "a.proto";
    importer.has_name = false;
    importer.dependency.push_back("b.proto");
    importer.dependency.push_back("b.proto");
    DescriptorProto unused = message_with_name("A");
    unused.field.push_back(int32_field("a", 1));
    append_message(importer, unused);

    FileDescriptorProto dependency;
    dependency.name = "b.proto";
    dependency.has_name = true;
    DescriptorProto b = message_with_name("B");
    b.field.push_back(int32_field("b", 1));
    append_message(dependency, b);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<FileDescriptorProto> files;
    files.push_back(importer);
    files.push_back(dependency);
    CHECK(easypb_schema::append_descriptor_files(files, "w48", set, error));
    CHECK(set.files().size() == 2);
    CHECK(set.files()[0]->file.has_name);
    CHECK(text(set.files()[0]->file.name) == "a.proto");

    easypb_schema::LinkedSchema linked;
    CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                      linked, error));
    CHECK(linked.files == 0);
    CHECK(error.file == "a.proto");
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(contains(error.message, "duplicate import \"b.proto\""));
}

void test_w48_package_bit(const std::string& data_dir)
{
    // L13: the provider declares a package string with has_package false.
    // Ingest normalizes the bit, so .pkg.M is indexed in .pkg and .M is not.
    const std::string provider_path = data_dir + "/w48-wire.pbs";
    easypb_schema::SchemaSet provider_set;
    easypb_schema::Diagnostic provider_error;
    CHECK(load_descriptor_set(provider_path, provider_set, provider_error));
    CHECK(provider_set.files().size() == 1);
    if (provider_set.files().size() != 1) return;
    easypb_schema::SchemaFile* provider = provider_set.files()[0].get();
    CHECK(provider->file.has_package);
    CHECK(text(provider->file.package) == "wire");

    // The decoded control already carries both bits; clear them, keeping the
    // strings, to make the hand-built shape the row is about.
    FileDescriptorProto provider_descriptor = provider->file;
    provider_descriptor.has_name = false;
    provider_descriptor.has_package = false;

    // The consumer names .wire.W and imports the provider.
    FileDescriptorProto consumer;
    consumer.name = "w48-consumer.proto";
    consumer.has_name = false;
    consumer.dependency.push_back("w48-wire.proto");
    DescriptorProto root = message_with_name("Root");
    root.field.push_back(named_field("w", 1, FieldDescriptorProto::TYPE_MESSAGE,
                                     ".wire.W"));
    append_message(consumer, root);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<FileDescriptorProto> files;
    files.push_back(provider_descriptor);
    files.push_back(consumer);
    CHECK(easypb_schema::append_descriptor_files(files, "w48-package", set,
                                                 error));
    CHECK(set.files().size() == 2);
    CHECK(set.files()[0]->file.has_package);
    CHECK(set.files()[1]->file.has_name);
    easypb_schema::LinkedSchema linked;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     error));
    CHECK(linked.files == &set);

    // Without the import the name is declared but not visible: code 6.
    FileDescriptorProto hidden_consumer = consumer;
    hidden_consumer.dependency.clear();
    easypb_schema::SchemaSet hidden_set;
    easypb_schema::Diagnostic hidden_error;
    std::vector<FileDescriptorProto> hidden_files;
    hidden_files.push_back(provider_descriptor);
    hidden_files.push_back(hidden_consumer);
    CHECK(easypb_schema::append_descriptor_files(hidden_files, "w48-hidden",
                                                 hidden_set, hidden_error));
    easypb_schema::LinkedSchema hidden_linked;
    CHECK(!easypb_schema::link_schema(hidden_set, easypb_schema::LINK_COMPLETE,
                                      hidden_linked, hidden_error));
    CHECK(hidden_linked.files == 0);
    CHECK(hidden_error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    CHECK(hidden_error.file == "w48-consumer.proto");

    // The global spelling .M no longer resolves: the declaration is .wire.W.
    FileDescriptorProto global_consumer = consumer;
    global_consumer.message_type[0].field[0].type_name = ".W";
    easypb_schema::SchemaSet global_set;
    easypb_schema::Diagnostic global_error;
    std::vector<FileDescriptorProto> global_files;
    global_files.push_back(provider_descriptor);
    global_files.push_back(global_consumer);
    CHECK(easypb_schema::append_descriptor_files(global_files, "w48-global",
                                                 global_set, global_error));
    easypb_schema::LinkedSchema global_linked;
    CHECK(!easypb_schema::link_schema(global_set, easypb_schema::LINK_COMPLETE,
                                      global_linked, global_error));
    CHECK(global_linked.files == 0);
    CHECK(global_error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
}

void test_w48_package_component()
{
    // L14: the package-component check reads the same normalized bit. A
    // hand-built file with has_package false still has a package, and its
    // invalid component is refused.
    FileDescriptorProto file;
    file.name = "bad-package.proto";
    file.has_name = true;
    file.package = "bad-name";
    file.has_package = false;
    DescriptorProto message = message_with_name("M");
    message.field.push_back(int32_field("a", 1));
    append_message(file, message);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<FileDescriptorProto> files(1, file);
    CHECK(easypb_schema::append_descriptor_files(files, "w48-component", set,
                                                 error));
    CHECK(set.files()[0]->file.has_package);
    easypb_schema::LinkedSchema linked;
    CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                      error));
    CHECK(linked.files == 0);
    CHECK(contains(error.message, "is not a valid identifier"));
}

void test_w48_wire_identity(const std::string& data_dir)
{
    // L15: the wire path is unchanged. The committed control carries both
    // bits; its twin clears them but keeps the strings. Ingest normalizes the
    // twin, so both produce the same header.
    std::string bytes;
    CHECK(read_binary_file(data_dir + "/w48-wire.pbs", bytes));
    const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(bytes);
    CHECK(decoded.file.size() == 1);
    if (decoded.file.size() != 1) return;
    CHECK(decoded.file[0].has_name);
    CHECK(decoded.file[0].has_package);
    const FileDescriptorProto wire = decoded.file[0];
    FileDescriptorProto twin = decoded.file[0];
    twin.has_name = false;
    twin.has_package = false;

    easypb_schema::SchemaSet wire_set;
    easypb_schema::Diagnostic wire_error;
    easypb_schema::LinkedSchema wire_linked;
    std::vector<FileDescriptorProto> wire_files(1, wire);
    CHECK(easypb_schema::append_descriptor_files(wire_files, "w48-wire",
                                                 wire_set, wire_error));
    CHECK(wire_set.files().size() == 1);
    if (wire_set.files().size() != 1) return;
    wire_set.add_target(*wire_set.files()[0]);
    CHECK(easypb_schema::link_schema(wire_set, easypb_schema::LINK_COMPLETE,
                                     wire_linked, wire_error));

    easypb_schema::SchemaSet twin_set;
    easypb_schema::Diagnostic twin_error;
    easypb_schema::LinkedSchema twin_linked;
    std::vector<FileDescriptorProto> twin_files(1, twin);
    CHECK(easypb_schema::append_descriptor_files(twin_files, "w48-twin",
                                                 twin_set, twin_error));
    CHECK(twin_set.files().size() == 1);
    if (twin_set.files().size() != 1) return;
    twin_set.add_target(*twin_set.files()[0]);
    CHECK(easypb_schema::link_schema(twin_set, easypb_schema::LINK_COMPLETE,
                                     twin_linked, twin_error));

    if (wire_linked.files != 0 && twin_linked.files != 0) {
        const std::string wire_header = easypb_codegen::generate_header(
            *wire_set.files()[0], wire_linked,
            easypb_codegen::GenerationOptions());
        const std::string twin_header = easypb_codegen::generate_header(
            *twin_set.files()[0], twin_linked,
            easypb_codegen::GenerationOptions());
        CHECK(wire_header == twin_header);
        CHECK(contains(wire_header, "namespace wire {"));
    }
}

void test_w48_identity_comparison()
{
    // L16: same_descriptor_file ignores the two bits (the strings are still
    // compared), and a different package string is still a difference.
    FileDescriptorProto left;
    left.name = "same.proto";
    left.has_name = false;
    left.package = "pkg";
    left.has_package = false;
    FileDescriptorProto right;
    right.name = "same.proto";
    right.has_name = true;
    right.package = "pkg";
    right.has_package = true;
    CHECK(easypb_schema::same_descriptor_file(left, right));

    right.package = "other";
    CHECK(!easypb_schema::same_descriptor_file(left, right));

    right.package = "pkg";
    right.name = "other.proto";
    CHECK(!easypb_schema::same_descriptor_file(left, right));
}

// --- W-47: the default rule reaches the library compatibility link ----------

void test_w47_library_compat()
{
    // L11: the in-memory compatibility link runs the same default rule as the
    // CLI. The link refuses; generate_files then has no published schema and
    // produces no files.
    FileDescriptorProto file;
    file.name = "l11.proto";
    file.has_name = true;
    file.package = "ext";
    file.has_package = true;
    file.syntax = "proto2";
    file.has_syntax = true;
    DescriptorProto root = message_with_name("Root");
    FieldDescriptorProto field = named_field(
        "x", 1, FieldDescriptorProto::TYPE_MESSAGE, ".ext.Missing");
    field.default_value = "x";
    field.has_default_value = true;
    root.field.push_back(field);
    append_message(file, root);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<FileDescriptorProto> files(1, file);
    CHECK(easypb_schema::append_descriptor_files(files, "l11", set, error));
    easypb_schema::LinkedSchema linked;
    CHECK(!easypb_schema::link_schema(
        set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
        error));
    CHECK(linked.files == 0);
    CHECK(error.file == "l11.proto");
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(contains(error.message, "message fields cannot have defaults"));

    std::vector<easypb_codegen::GeneratedFile> output;
    easypb_codegen::GenerationOptions options;
    CHECK(!easypb_codegen::generate_files(linked, options, output, error));
    CHECK(output.empty());
    CHECK(contains(error.message, "invalid empty state"));
}

// --- W-58: one name, one meaning --------------------------------------------

void test_w58_local_only()
{
    // L17: a complete link is local by construction.
    {
        FileDescriptorProto file;
        make_simple_file(file, "l17.proto", "p");
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        std::vector<FileDescriptorProto> files(1, file);
        CHECK(easypb_schema::append_descriptor_files(files, "l17", set, error));
        easypb_schema::LinkedSchema linked;
        CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                         linked, error));
        CHECK(linked.files == &set);
        CHECK(linked.local_only);
    }
    // L18: compatibility mode over a self-contained thin descriptor.
    {
        FileDescriptorProto file;
        make_simple_file(file, "l18.proto", "p");
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        std::vector<FileDescriptorProto> files(1, file);
        CHECK(easypb_schema::append_descriptor_files(files, "l18", set, error));
        easypb_schema::LinkedSchema linked;
        CHECK(easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
            error));
        CHECK(linked.files == &set);
        CHECK(linked.local_only);
        CHECK(set.files()[0]->warnings.empty());
    }
    // L19: compatibility mode with a null-target import edge.
    {
        FileDescriptorProto file;
        make_simple_file(file, "l19.proto", "p");
        file.dependency.push_back("gone.proto");
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        std::vector<FileDescriptorProto> files(1, file);
        CHECK(easypb_schema::append_descriptor_files(files, "l19", set, error));
        easypb_schema::LinkedSchema linked;
        CHECK(easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
            error));
        CHECK(linked.files == &set);
        CHECK(!linked.local_only);
        CHECK(!set.files()[0]->edges.empty());
        CHECK(set.files()[0]->edges[0].target == 0);
    }
    // L20: compatibility mode with a tolerated external reference.
    {
        FileDescriptorProto file;
        make_simple_file(file, "l20.proto", "p");
        file.message_type[0].field.push_back(named_field(
            "t", 2, FieldDescriptorProto::TYPE_MESSAGE, ".ext.T"));
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        std::vector<FileDescriptorProto> files(1, file);
        CHECK(easypb_schema::append_descriptor_files(files, "l20", set, error));
        easypb_schema::LinkedSchema linked;
        CHECK(easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
            error));
        CHECK(linked.files == &set);
        CHECK(!linked.local_only);
        CHECK(set.files()[0]->warnings.size() == 1);
    }
    // L21: a failed link leaves the documented invalid empty state.
    {
        FileDescriptorProto file;
        make_simple_file(file, "l21.proto", "p");
        file.message_type[0].field.push_back(named_field(
            "t", 2, FieldDescriptorProto::TYPE_MESSAGE, ".missing.T"));
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        std::vector<FileDescriptorProto> files(1, file);
        CHECK(easypb_schema::append_descriptor_files(files, "l21", set, error));
        easypb_schema::LinkedSchema linked;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
}

// --- the emit mode for the build's compile check ----------------------------

bool emit_options_default(const std::string& data_dir, const std::string& path)
{
    easypb_schema::SchemaSet set;
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    if (!load_single(data_dir + "/options-default.pbs", set, linked, error)) {
        std::cerr << "emit-options-default: " << error.message << '\n';
        return false;
    }
    try {
        const std::string header = easypb_codegen::generate_header(
            *set.files()[0], linked, easypb_codegen::GenerationOptions());
        if (!write_binary_file(path, header)) {
            std::cerr << "emit-options-default: cannot write " << path << '\n';
            return false;
        }
    } catch (const std::exception& problem) {
        std::cerr << "emit-options-default: " << problem.what() << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string emit_path;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--emit-options-default") {
            if (++i >= argc) {
                std::cerr << "--emit-options-default needs a path\n";
                return EXIT_FAILURE;
            }
            emit_path = argv[i];
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 1) {
        std::cerr << "usage: test_library <data-dir>\n"
                     "       test_library --emit-options-default <path> "
                     "<data-dir>\n";
        return EXIT_FAILURE;
    }
    const std::string data_dir = positional[0];

    if (!emit_path.empty()) {
        return emit_options_default(data_dir, emit_path) ? EXIT_SUCCESS
                                                         : EXIT_FAILURE;
    }

    test_w11(data_dir);
    test_w48_name_bit();
    test_w48_package_bit(data_dir);
    test_w48_package_component();
    test_w48_wire_identity(data_dir);
    test_w48_identity_comparison();
    test_w47_library_compat();
    test_w58_local_only();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all library API tests passed\n";
    return EXIT_SUCCESS;
}
