// Tests for descriptor ingestion: the common adapter
// (codegen/descriptor_adapter.*) and the standalone `.pbs` container wrapper
// (codegen/descriptor_loader.*). This file links no parser and no CLI, so it
// must keep passing in descriptor-set-only builds
// (EASYPB_CODEGEN_WITH_PROTO_PARSER=OFF); it links easypb_codegen_core only
// for the lifetime test's generation half.
//
// The invariants this suite witnesses (plan 5 process package,
// build/plan5-codegen-integration-process/invariants.md, group D):
//   D1 inputs are deep-copied; buffers may be destroyed after ingestion
//   D2 every retained string is interned in the owning pool
//   D3 duplicate logical names are rejected within one container and across
//      different definitions; an identical copy from another container merges
//      (plan 5 follow-up S5/W-36)
//   D4 ingestion never selects targets
//   D5 descriptor origin is labeled without inventing a file
//   D6 no common operation requires physical_name to exist on disk
// D7 (the common library performs no file access) is an inspection row owned
// by the reviewer: this test's own link inputs are its fixture -- the adapter
// is inside easypb_schema, the container reader outside it.
//
// Fixtures are the tracked `.pbs` corpus, reached through the tests-source
// directory argv[1] passes (the pattern codegen.imports.linker uses), plus
// hand-built wire sets written to the working directory for the shapes the
// corpus does not carry (an unnamed file, a duplicate inside one container).
// The hand-built encoder is independent of easypb's own decoder, for the same
// reason test_linker.cpp's is: a helper shared with the reader could cancel a
// wrong field number out. Field numbers are descriptor.proto's own.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "descriptor_adapter.hpp"
#include "descriptor_loader.hpp"
#include "generation.hpp"
#include "generation_options.hpp"
#include "schema.hpp"
#include "schema_linker.hpp"

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

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

// Reads one binary fixture whole. The caller keeps the string on its own
// stack frame: in C++17 the decoder's str_views point into this buffer, and
// the lifetime tests below depend on being able to destroy it after
// ingestion.
bool read_binary_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

// The working directory is pinned to the test binary's build directory by
// tests/CMakeLists.txt, so scratch containers never touch the source tree.
bool write_binary_file(const std::string& path, const std::string& contents)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) return false;
    output.write(contents.data(), std::streamsize(contents.size()));
    output.close();
    return !output.fail();
}

void wire_varint(std::string& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(char(value));
}

std::string wire_tag(int field, unsigned wire)
{
    std::string out;
    wire_varint(out, (std::uint64_t(field) << 3) | wire);
    return out;
}

std::string wire_text_field(int field, const char* value)
{
    const std::string payload(value);
    std::string out = wire_tag(field, 2);
    wire_varint(out, payload.size());
    out += payload;
    return out;
}

std::string wire_message_field(int field, const std::string& payload)
{
    std::string out = wire_tag(field, 2);
    wire_varint(out, payload.size());
    out += payload;
    return out;
}

// --- hand-built containers -------------------------------------------------

// A varint-valued descriptor field (numbers, labels, kinds, indexes).
std::string wire_uint_field(int field, std::uint64_t value)
{
    std::string out = wire_tag(field, 0);
    wire_varint(out, value);
    return out;
}

// A FileDescriptorProto with only a logical name: the smallest named file the
// adapter accepts, enough for the duplicate-name rows.
std::string named_file(const char* logical_name)
{
    return wire_text_field(1, logical_name);
}

// One set, two named files: the caller picks whether the names collide.
std::string two_file_set(const char* first_name, const char* second_name)
{
    std::string set = wire_message_field(1, named_file(first_name));
    set += wire_message_field(1, named_file(second_name));
    return set;
}

// One unnamed file: package `legacy` and one message. The legacy single-file
// stdout compatibility shape, which only the loader may ingest.
std::string anonymous_file_set()
{
    std::string file = wire_text_field(2, "legacy");
    file += wire_message_field(4, wire_text_field(1, "Legacy"));
    return wire_message_field(1, file);
}

// Two files, the second unnamed: legal per-file at the adapter and refused by
// the loader, because only a SINGLE-file set may omit the logical name.
std::string mixed_unnamed_file_set()
{
    std::string named = wire_text_field(1, "named.proto");
    named += wire_text_field(2, "named");
    std::string unnamed = wire_text_field(2, "legacy");
    std::string set = wire_message_field(1, named);
    set += wire_message_field(1, unnamed);
    return set;
}

// V-14: one named file whose dependency list is always the same three names,
// with the public and weak index lists written in the requested order. The
// storage order of the index lists carries no meaning; one logical file may be
// carried by two containers that spelled it differently.
std::string dependency_merge_set(bool reversed_public, bool different_weak)
{
    std::string file = wire_text_field(1, "deps-merge.proto");
    file += wire_text_field(3, "a.proto");
    file += wire_text_field(3, "b.proto");
    file += wire_text_field(3, "c.proto");
    file += wire_uint_field(10, reversed_public ? 2 : 0);
    file += wire_uint_field(10, reversed_public ? 0 : 2);
    file += wire_uint_field(11, different_weak ? 0 : 1);
    return wire_message_field(1, file);
}

// V-42: one named file with a single int32 field, and the label written in the
// requested value. `with_label = false` is protoc's own shape for a field whose
// label was routed to the unknown fields; a crafted container can spell 99.
std::string label_set(std::uint64_t label, bool with_label)
{
    std::string field = wire_text_field(1, "a");
    field += wire_uint_field(3, 1);
    if (with_label) field += wire_uint_field(4, label);
    field += wire_uint_field(5, 5);  // TYPE_INT32
    std::string message = wire_text_field(1, "M");
    message += wire_message_field(2, field);
    std::string file = wire_text_field(1, "label-merge.proto");
    file += wire_text_field(2, "p");
    file += wire_message_field(4, message);
    file += wire_text_field(12, "proto2");
    return wire_message_field(1, file);
}

// `package p; message M { repeated int32 f = 1; }` spelled with the presence
// bits a hand-encoded copy may write differently from protoc's own encoding.
// The loader's identity test must read the effective content, not these bits
// (CR-24, W-36).
struct SemanticSpelling
{
    SemanticSpelling()
        : explicit_proto2(false), write_json_name(true),
          write_empty_options(false), write_packed_false(false),
          write_empty_cpp_group(false), write_empty_cpp_type(false),
          proto3_optional(false), package("p"), field_number(1) {}

    bool explicit_proto2;    // protoc omits `syntax` for a proto2 file
    bool write_json_name;    // protoc always writes the derived name
    bool write_empty_options; // an empty FieldOptions message
    bool write_packed_false;  // `packed = false`, the proto2 default
    bool write_empty_cpp_group; // FieldOptions { [easypb.cpp] {} }
    bool write_empty_cpp_type;  // [easypb.cpp].type present but empty
    bool proto3_optional;     // D1-A presence flag (wire field 17)
    const char* package;
    int field_number;
};

std::string semantic_file(const SemanticSpelling& spelling)
{
    std::string field = wire_text_field(1, "f");
    field += wire_uint_field(3, static_cast<std::uint64_t>(spelling.field_number));
    field += wire_uint_field(4, 3); // LABEL_REPEATED
    field += wire_uint_field(5, 5); // TYPE_INT32
    if (spelling.write_empty_options) {
        field += wire_message_field(8, std::string());
    } else if (spelling.write_packed_false) {
        field += wire_message_field(8, wire_uint_field(2, 0));
    } else if (spelling.write_empty_cpp_group) {
        // The extension group is present with no member: no type override.
        field += wire_message_field(
            8, wire_message_field(EASYPB_CPP_FIELD_OPTIONS_NUMBER,
                                  std::string()));
    } else if (spelling.write_empty_cpp_type) {
        // The type member is present with empty text: a request the source
        // parser and the generator both refuse.
        field += wire_message_field(
            8, wire_message_field(EASYPB_CPP_FIELD_OPTIONS_NUMBER,
                                  wire_text_field(1, "")));
    }
    if (spelling.write_json_name) field += wire_text_field(10, "f");
    // D1-A: the presence flag is modeled content, so a copy that carries it
    // and one that does not are two different definitions (see the identity
    // test below).
    if (spelling.proto3_optional) field += wire_uint_field(17, 1);
    std::string message = wire_text_field(1, "M");
    message += wire_message_field(2, field);
    std::string file = wire_text_field(1, "semantic-merge.proto");
    file += wire_text_field(2, spelling.package);
    file += wire_message_field(4, message);
    if (spelling.explicit_proto2) file += wire_text_field(12, "proto2");
    return wire_message_field(1, file);
}

const FieldDescriptorProto* find_field(const easypb_schema::SchemaFile& file,
                                       const char* message_name,
                                       const char* field_name)
{
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        const DescriptorProto& message = file.file.message_type[i];
        if (text(message.name) != message_name) continue;
        for (std::size_t j = 0; j < message.field.size(); ++j) {
            if (text(message.field[j].name) == field_name) {
                return &message.field[j];
            }
        }
    }
    return 0;
}

void check_cpp_type(const easypb_schema::SchemaFile& file,
                    const char* message_name, const char* field_name,
                    const char* expected)
{
    const FieldDescriptorProto* field =
        find_field(file, message_name, field_name);
    CHECK(field != 0);
    if (field == 0) return;
    CHECK(text(field->options.cpp.type) == expected);
}

// Generate one linked target with default options, the way the standalone
// publisher does after option finalization. Batch 3 replaced the single-file
// entry point with the linked generator, so the lifetime proofs now compare a
// poisoned-buffer ingestion against a fresh one through that same entry
// point: a string the adapter failed to intern would surface as a byte
// difference, not as a crash.
std::string generate_linked_one(const easypb_schema::SchemaFile& target,
                                const easypb_schema::LinkedSchema& linked)
{
    easypb_codegen::GenerationOptions options;
    std::string option_error;
    CHECK(easypb_codegen::finalize_generation_options(options, option_error));
    return easypb_codegen::generate_header(target, linked, options);
}

// --- the common adapter ----------------------------------------------------

// D3, empty half: an empty input list is a caller error, not a no-op. It goes
// through the same failure path as every other refusal, so a pre-populated set
// is cleared too -- a caller must not keep views across a failed ingestion.
void test_adapter_rejects_an_empty_input()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& pre = set.add_file("pre.proto");
    pre.from_descriptor_set = true;

    easypb_schema::Diagnostic error;
    const std::vector<FileDescriptorProto> none;
    CHECK(!easypb_schema::append_descriptor_files(none, "empty-input", set,
                                                  error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.file == "empty-input");
    CHECK(error.message.find("contains no FileDescriptorProto") !=
          std::string::npos);
    CHECK(error.location.line == 0);
    CHECK(error.location.column == 0);
    CHECK(set.files().empty());
    CHECK(set.targets().empty());
}

// The adapter requires a real logical name on every file (dependencies and
// target selection address files by name). The anonymous single-file shape is
// the loader's compatibility path alone.
void test_adapter_rejects_an_unnamed_file()
{
    const std::string bytes = anonymous_file_set();
    const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(bytes);
    CHECK(decoded.file.size() == 1);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::append_descriptor_files(decoded.file, "anonymous.pbs",
                                                  set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.file == "anonymous.pbs");
    CHECK(error.message.find("has no logical name") != std::string::npos);
    CHECK(error.message.find("single-file") != std::string::npos);
    CHECK(set.files().empty());
}

// D3: one name, two declarations. Rejected within a single call and against
// everything appended earlier; the control in the same test proves the
// refusal is about the name and not about batch size, and both failures leave
// the set empty.
void test_adapter_rejects_duplicate_names_within_and_across_calls()
{
    // Control: two distinct names in one call are appended.
    {
        const std::string bytes = two_file_set("a.proto", "b.proto");
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 2);

        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::append_descriptor_files(decoded.file, "pair", set,
                                                     error));
        CHECK(set.files().size() == 2);

        // Across calls: the same logical name again is refused, and the
        // failure clears the files appended before it.
        CHECK(!easypb_schema::append_descriptor_files(decoded.file,
                                                      "pair-again", set,
                                                      error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("\"a.proto\"") != std::string::npos);
        CHECK(set.files().empty());
    }

    // Within one call: nothing is appended before the names are validated,
    // and the failure clears the set regardless.
    {
        const std::string bytes = two_file_set("dup.proto", "dup.proto");
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 2);

        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::append_descriptor_files(decoded.file, "dup-set",
                                                      set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("\"dup.proto\"") != std::string::npos);
        CHECK(set.files().empty());
        CHECK(set.targets().empty());
    }
}

// D1/D2/D4/D5/D6: decode a real container, ingest it through the common
// adapter, then poison and destroy the only buffer every borrowed view
// pointed into. Everything below -- indexing and generation -- must work from
// the owned copy alone, and generation must equal generation from a fresh
// ingestion of the same bytes, so a single missed interning cannot hide
// behind freed-but-intact memory.
void test_adapter_owns_a_decoded_buffer_and_generates_after_destruction(
    const std::string& fixture_dir)
{
    const std::string path =
        fixture_dir + "/codegen/parser/data/integration-multi.pbs";
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    {
        std::string bytes;
        CHECK(read_binary_file(path, bytes));
        {
            const FileDescriptorSet decoded =
                easypb::decode<FileDescriptorSet>(bytes);
            CHECK(decoded.file.size() == 2);
            if (decoded.file.size() != 2) return;
            CHECK(easypb_schema::append_descriptor_files(decoded.file, path,
                                                         set, error));
        }
        // The borrowed views die here, twice over: the bytes are poisoned so
        // a view into freed-but-intact memory cannot read what it used to,
        // then the buffer itself goes away.
        if (!bytes.empty()) {
            std::memset(&bytes[0], '\xA5', bytes.size());
        }
        bytes.clear();
        bytes.shrink_to_fit();
    }

    CHECK(set.files().size() == 2);
    CHECK(set.targets().empty());  // D4: ingestion never selects targets.
    if (set.files().size() != 2) return;
    const easypb_schema::SchemaFile& first = *set.files()[0];
    const easypb_schema::SchemaFile& second = *set.files()[1];
    CHECK(first.from_descriptor_set);   // D5
    CHECK(second.from_descriptor_set);
    CHECK(first.physical_name.empty());  // D6
    CHECK(second.physical_name.empty());

    // D2: every string class the corpus carries, read after the buffer died.
    CHECK(text(first.file.name) == "integration-proto2.proto");
    CHECK(text(first.file.package) == "integration.proto2");
    CHECK(first.file.syntax.empty());
    CHECK(!first.file.has_syntax);
    CHECK(first.file.message_type.size() == 1);
    if (first.file.message_type.size() == 1) {
        const DescriptorProto& message = first.file.message_type[0];
        CHECK(text(message.name) == "Proto2Message");
        CHECK(text(message.field[0].name) == "id");
        CHECK(message.field[0].has_default_value);  // presence flags travel
        CHECK(text(message.field[0].default_value) == "7");
        CHECK(text(message.field[1].default_value) == "default-name");
        CHECK(text(message.field[4].type_name) ==
              ".integration.proto2.Proto2Message.CountsEntry");
        CHECK(text(message.nested_type[0].name) == "Nested");
        CHECK(text(message.nested_type[0].field[0].name) == "value");
        CHECK(text(message.enum_type[0].name) == "LocalState");
        CHECK(text(message.enum_type[0].value[1].name) == "LOCAL_ONE");
        CHECK(text(message.oneof_decl[0].name) == "choice");
    }
    CHECK(text(second.file.name) == "integration-proto3.proto");
    CHECK(text(second.file.package) == "integration.proto3");
    CHECK(text(second.file.syntax) == "proto3");
    CHECK(text(second.file.enum_type[0].name) == "State");
    CHECK(text(second.file.enum_type[0].value[0].name) == "STATE_ZERO");
    CHECK(text(second.file.message_type[1].name) == "Proto3Message");
    CHECK(text(second.file.message_type[1].field[5].type_name) ==
          ".integration.proto3.Child");

    // D1, indexing half: the linker runs with every borrowed buffer gone.
    easypb_schema::LinkedSchema linked;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     error));
    CHECK(linked.files == &set);
    CHECK(linked.local_only);

    // D1, generation half: the poisoned-buffer ingestion generates the same
    // bytes as a fresh ingestion of the same container through the same
    // linked entry point, so every string the generator reads survived the
    // buffer's death.
    const std::string from_owned = generate_linked_one(first, linked);
    CHECK(!from_owned.empty());
    {
        easypb_schema::SchemaSet live_set;
        std::vector<std::string> live_inputs(1, path);
        CHECK(easypb_schema::load_descriptor_sets(live_inputs, live_set, error));
        easypb_schema::LinkedSchema live_linked;
        CHECK(easypb_schema::link_schema(live_set, easypb_schema::LINK_COMPLETE,
                                         live_linked, error));
        easypb_schema::SchemaFile* live_target =
            live_set.find_file("integration-proto2.proto");
        CHECK(live_target != 0);
        if (live_target != 0) {
            CHECK(from_owned == generate_linked_one(*live_target, live_linked));
        }
    }
}

// W-43/D2: the newly modeled `json_name` is interned like every other
// retained string. Read it after the decoded buffer was poisoned.
void test_adapter_owns_json_name(const std::string& fixture_dir)
{
    const std::string path =
        fixture_dir + "/codegen/validity/data/json-names-distinct.pbs";
    std::string bytes;
    CHECK(read_binary_file(path, bytes));
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    {
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        if (decoded.file.size() != 1) return;
        CHECK(easypb_schema::append_descriptor_files(decoded.file, path, set,
                                                     error));
    }
    if (!bytes.empty()) {
        std::memset(&bytes[0], '\xA5', bytes.size());
    }
    bytes.clear();
    bytes.shrink_to_fit();

    CHECK(set.files().size() == 1);
    if (set.files().size() != 1) return;
    const easypb_schema::SchemaFile& file = *set.files()[0];
    const FieldDescriptorProto* a =
        find_field(file, "JsonNamesDistinct", "a");
    const FieldDescriptorProto* b =
        find_field(file, "JsonNamesDistinct", "b");
    CHECK(a != 0 && a->has_json_name);
    CHECK(b != 0 && b->has_json_name);
    CHECK(a != 0 && text(a->json_name) == "alpha");
    CHECK(b != 0 && text(b->json_name) == "beta");
}

// CR-06/D2: the newly modeled `reserved_name` strings are interned like every
// other retained string. Each container is decoded, ingested, then its buffer
// is poisoned and destroyed; the message and the enum halves prove both
// interning walks (intern_message and intern_enum).
void test_adapter_owns_reserved_names(const std::string& fixture_dir)
{
    // message M { reserved "a", "a"; } - the duplicate is a semantic defect,
    // not an ingestion one, so both views must survive.
    {
        const std::string path =
            fixture_dir + "/codegen/parity/data/res-dup-name.pbs";
        std::string bytes;
        CHECK(read_binary_file(path, bytes));
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        {
            const FileDescriptorSet decoded =
                easypb::decode<FileDescriptorSet>(bytes);
            CHECK(decoded.file.size() == 1);
            if (decoded.file.size() != 1) return;
            CHECK(easypb_schema::append_descriptor_files(decoded.file, path,
                                                         set, error));
        }
        if (!bytes.empty()) {
            std::memset(&bytes[0], '\xA5', bytes.size());
        }
        bytes.clear();
        bytes.shrink_to_fit();

        CHECK(set.files().size() == 1);
        if (set.files().size() != 1) return;
        const easypb_schema::SchemaFile& file = *set.files()[0];
        CHECK(file.file.message_type.size() == 1);
        if (file.file.message_type.size() == 1) {
            const DescriptorProto& message = file.file.message_type[0];
            CHECK(message.reserved_name.size() == 2);
            if (message.reserved_name.size() == 2) {
                CHECK(text(message.reserved_name[0]) == "a");
                CHECK(text(message.reserved_name[1]) == "a");
            }
        }
    }
    // enum E { reserved "A"; } - the enum walk keeps its own reserved names.
    {
        const std::string path =
            fixture_dir + "/codegen/parity/data/enum-reserved-name.pbs";
        std::string bytes;
        CHECK(read_binary_file(path, bytes));
        easypb_schema::SchemaSet set;
        easypb_schema::Diagnostic error;
        {
            const FileDescriptorSet decoded =
                easypb::decode<FileDescriptorSet>(bytes);
            CHECK(decoded.file.size() == 1);
            if (decoded.file.size() != 1) return;
            CHECK(easypb_schema::append_descriptor_files(decoded.file, path,
                                                         set, error));
        }
        if (!bytes.empty()) {
            std::memset(&bytes[0], '\xA5', bytes.size());
        }
        bytes.clear();
        bytes.shrink_to_fit();

        CHECK(set.files().size() == 1);
        if (set.files().size() != 1) return;
        const easypb_schema::SchemaFile& file = *set.files()[0];
        CHECK(file.file.enum_type.size() == 1);
        if (file.file.enum_type.size() == 1) {
            const EnumDescriptorProto& enumeration = file.file.enum_type[0];
            CHECK(enumeration.reserved_name.size() == 1);
            if (enumeration.reserved_name.size() == 1) {
                CHECK(text(enumeration.reserved_name[0]) == "A");
            }
        }
    }
}

// --- the standalone container wrapper --------------------------------------

// D5/D6: a multi-file container is addressed by container path, and each file
// is ingested under its own logical name after the container bytes are gone
// (they die inside load_descriptor_sets). physical_name may retain the
// container path; nothing reads it as a disk path.
void test_loader_loads_a_complete_container(const std::string& fixture_dir)
{
    const std::string path =
        fixture_dir + "/codegen/parser/data/integration-multi.pbs";
    std::vector<std::string> inputs(1, path);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 2);
    CHECK(set.targets().empty());
    if (set.files().size() != 2) return;
    for (std::size_t i = 0; i < set.files().size(); ++i) {
        const easypb_schema::SchemaFile& file = *set.files()[i];
        CHECK(file.from_descriptor_set);
        CHECK(file.physical_name == path);
        CHECK(set.find_file(text(file.file.name)) == &file);
    }
    // D4, selection half: ingestion selected nothing; only an explicit caller
    // choice populates targets, in first-requested order.
    set.add_target(*set.files()[1]);
    set.add_target(*set.files()[0]);
    CHECK(set.targets().size() == 2);
    CHECK(set.targets()[0] == set.files()[1].get());
    CHECK(set.targets()[1] == set.files()[0].get());
    // The container is not needed again: the loaded copy links on its own.
    easypb_schema::LinkedSchema linked;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     error));
    CHECK(linked.local_only);
}

// An empty container is refused, not quietly ignored: zero FileDescriptorProto
// in a `.pbs` file is a malformed input, and the failure clears the set. No
// input at all is a caller error through the same path.
void test_loader_rejects_an_empty_container(const std::string& fixture_dir)
{
    const std::string path = fixture_dir + "/codegen/parser/data/empty.pbs";
    std::vector<std::string> inputs(1, path);
    easypb_schema::SchemaSet set;
    set.add_file("pre.proto");
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.file == path);
    CHECK(error.message.find("contains no FileDescriptorProto") !=
          std::string::npos);
    CHECK(set.files().empty());

    std::vector<std::string> none;
    CHECK(!easypb_schema::load_descriptor_sets(none, set, error));
    CHECK(error.message.find("at least one") != std::string::npos);
    CHECK(set.files().empty());
}

// Only a SINGLE-file container may omit the logical name; a multi-file one
// with an unnamed member is refused before anything is appended.
void test_loader_rejects_an_unnamed_file_in_a_multi_file_container()
{
    const std::string scratch = "imports-descriptor-mixed.pbs";
    CHECK(write_binary_file(scratch, mixed_unnamed_file_set()));
    std::vector<std::string> inputs(1, scratch);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.file == scratch);
    CHECK(error.message.find("only a single-file set may omit it") !=
          std::string::npos);
    CHECK(set.files().empty());
    std::remove(scratch.c_str());
}

// W-57: the tracked anonymous fixture through the public loader. The container
// path is physical metadata only: the descriptor has no name and no name bit,
// and the identity is not in the set's index. The identity is also never
// interned into the file's pool - a fact the public API cannot observe, but
// the code path (add_unnamed_file, not add_file) is the proof.
void test_loader_anonymous_fixture_keeps_no_identity(
    const std::string& fixture_dir)
{
    const std::string path = fixture_dir + "/codegen/cli/data/anon.pbs";
    std::vector<std::string> inputs(1, path);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 1);
    CHECK(set.targets().empty());
    if (set.files().size() != 1) return;
    const easypb_schema::SchemaFile& slot = *set.files()[0];
    CHECK(slot.from_descriptor_set);
    CHECK(!slot.file.has_name);
    CHECK(slot.file.name.empty());
    CHECK(slot.physical_name == path);
    CHECK(set.find_file(path) == 0);
    CHECK(set.find_file("") == 0);
}

// D5: a container holding exactly one unnamed file keeps the legacy stdout
// compatibility path. The container path is an internal identity that labels
// diagnostics and is never written into the copied descriptor; it is not a
// logical name, so after the ingest it is not addressable through the set
// (S4/W-18: `--target <container>` must not select the unnamed file).
void test_loader_single_anonymous_file_is_not_addressable()
{    const std::string scratch = "imports-descriptor-anonymous.pbs";
    CHECK(write_binary_file(scratch, anonymous_file_set()));
    std::vector<std::string> inputs(1, scratch);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 1);
    CHECK(set.targets().empty());
    if (set.files().size() == 1) {
        const easypb_schema::SchemaFile* slot = set.files()[0].get();
        CHECK(slot != 0);
        if (slot != 0) {
            CHECK(slot->from_descriptor_set);
            CHECK(slot->file.name.empty());
            CHECK(!slot->file.has_name);
            CHECK(set.find_file(scratch) == 0);
            CHECK(set.find_file("") == 0);
            // The rest of the file is interned normally: the package and the
            // message name survive the container bytes.
            CHECK(text(slot->file.package) == "legacy");
            CHECK(slot->file.message_type.size() == 1);
            if (slot->file.message_type.size() == 1) {
                CHECK(text(slot->file.message_type[0].name) == "Legacy");
            }
            CHECK(slot->physical_name == scratch);
        }
    }
    std::remove(scratch.c_str());
}

// D3 through the wrapper: duplicates within one container are refused, and
// the same set with distinct names is accepted as the control -- the refusal
// is about the name, not the container. A failed load leaves the set usable.
void test_loader_rejects_duplicate_names_within_one_container()
{
    const std::string duplicate = "imports-descriptor-duplicate.pbs";
    const std::string distinct = "imports-descriptor-distinct.pbs";
    CHECK(write_binary_file(duplicate, two_file_set("dup.proto", "dup.proto")));
    CHECK(write_binary_file(distinct, two_file_set("a.proto", "b.proto")));

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<std::string> inputs(1, duplicate);
    set.add_file("pre.proto");
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(error.file == duplicate);
    CHECK(error.message.find("\"dup.proto\"") != std::string::npos);
    CHECK(set.files().empty());  // the failure cleared the set

    inputs[0] = distinct;
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 2);
    std::remove(duplicate.c_str());
    std::remove(distinct.c_str());
}

// D3 across containers, under W-36: one logical file several containers carry
// is one file. An identical copy is merged (the load succeeds and the set keeps
// one file), while two DIFFERENT definitions of the same logical name are
// refused - silently keeping one of them would generate a header from a schema
// the caller may not have meant - and the diagnostic names both containers.
void test_loader_merges_duplicate_names_across_containers(
    const std::string& fixture_dir)
{
    const std::string path =
        fixture_dir + "/codegen/parser/data/integration-proto2.pbs";
    std::vector<std::string> once(1, path);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::load_descriptor_sets(once, set, error));
    CHECK(set.files().size() == 1);
    // The same container again: its one file is identical, so it is merged and
    // the set stays usable with the first copy.
    CHECK(easypb_schema::load_descriptor_sets(once, set, error));
    CHECK(set.files().size() == 1);
    CHECK(set.find_file("integration-proto2.proto") != 0);

    // Two containers carrying one logical name, in a single call: merged.
    std::vector<std::string> twice(2, path);
    CHECK(easypb_schema::load_descriptor_sets(twice, set, error));
    CHECK(set.files().size() == 1);

    // The conflict half: a second container defines the SAME logical name with
    // a different package, which no merge can reconcile.
    const std::string conflicting = "imports-descriptor-conflict.pbs";
    std::string changed;
    changed += wire_message_field(
        1, named_file("integration-proto2.proto") +
               wire_text_field(2, "conflict"));
    CHECK(write_binary_file(conflicting, changed));
    std::vector<std::string> inputs;
    inputs.push_back(path);
    inputs.push_back(conflicting);
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(error.file == conflicting);
    CHECK(error.message.find("\"integration-proto2.proto\"") !=
          std::string::npos);
    CHECK(error.message.find(path) != std::string::npos);
    // W-19/W-29: the conflicting container IS the diagnostic's file
    // (`error.file`, checked above), so the message names it once - as the
    // label - and not a second time in its text.
    CHECK(error.message.find(conflicting) == std::string::npos);
    CHECK(error.message.find(conflicting + ": ") != 0);
    CHECK(set.files().empty());
    CHECK(set.targets().empty());
    std::remove(conflicting.c_str());
}

// CR-24, the semantic half of W-36: the identity test reads the content the
// linker reads, not the presence bits a hand-encoded copy may spell
// differently. protoc 30.1 merges every pair below (measured 2026-10-25 with
// `--descriptor_set_in=<a>;<b> --descriptor_set_out=out.pbs
// semantic-merge.proto`: exit 0 for the absent-vs-explicit `syntax = "proto2"`
// pair and for the derived-vs-omitted `json_name` pair), and a real difference
// is still refused.
void test_loader_merges_semantically_equal_copies()
{
    const std::string base = "cr24-protoc-spelling.pbs";
    const std::string variant = "cr24-variant.pbs";
    SemanticSpelling protoc_like;
    CHECK(write_binary_file(base, semantic_file(protoc_like)));

    // One variant per non-identity presence bit the merge used to refuse.
    // U-07 adds the `(easypb.cpp)` group with no `type` member: both the group
    // and its member must be present for a custom type to exist, so the empty
    // group is the unset request.
    SemanticSpelling spellings[5];
    spellings[0].explicit_proto2 = true;
    spellings[1].write_json_name = false;
    spellings[2].write_packed_false = true;
    spellings[3].write_empty_options = true;
    spellings[4].write_empty_cpp_group = true;

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(write_binary_file(variant, semantic_file(spellings[i])));
        std::vector<std::string> inputs;
        inputs.push_back(base);
        inputs.push_back(variant);
        CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
        CHECK(set.files().size() == 1);
        CHECK(set.find_file("semantic-merge.proto") != 0);
    }

    // The two differences the linker does read stay refusals: a different
    // package and a different field number are two different files.
    SemanticSpelling different_package;
    different_package.package = "q";
    SemanticSpelling different_number;
    different_number.field_number = 2;
    // D1-A: `proto3_optional` is presence, so the two copies describe
    // different fields and must not merge.
    SemanticSpelling different_presence;
    different_presence.proto3_optional = true;
    // U-07: a present-but-empty `(easypb.cpp).type` is not the unset request.
    // The source parser and the generator refuse that spelling (`must not be
    // empty`), so the merge must refuse the pair instead of choosing the copy
    // that hides the refusal.
    SemanticSpelling different_cpp_type;
    different_cpp_type.write_empty_cpp_type = true;
    const SemanticSpelling* differences[4] = {
        &different_package, &different_number, &different_presence,
        &different_cpp_type};
    for (std::size_t i = 0; i < 4; ++i) {
        const SemanticSpelling& spelling = *differences[i];
        CHECK(write_binary_file(variant, semantic_file(spelling)));
        std::vector<std::string> inputs;
        inputs.push_back(base);
        inputs.push_back(variant);
        CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("\"semantic-merge.proto\"") !=
              std::string::npos);
        CHECK(set.files().empty());
    }
    std::remove(base.c_str());
    std::remove(variant.c_str());
}

// U-52: identity for the unsupported syntax class includes the edition
// CARRIER. A file whose class comes from `edition` and one whose class comes
// from the `syntax` string are two different definitions even when the stored
// edition value agrees; comparing only the value would merge the pair and then
// silently keep one copy. The comparison must answer the same way in both
// orders.
void test_identity_compares_the_edition_carrier()
{
    FileDescriptorProto from_edition;
    from_edition.name = "edition-carrier.proto";
    from_edition.syntax = "editions";
    from_edition.has_syntax = true;
    from_edition.edition = 0;  // EDITION_UNKNOWN, an unsupported edition
    from_edition.has_edition = true;

    FileDescriptorProto from_syntax;
    from_syntax.name = "edition-carrier.proto";
    from_syntax.syntax = "editions";
    from_syntax.has_syntax = true;
    from_syntax.has_edition = false;

    CHECK(easypb_schema::classify_file_syntax(from_edition) ==
          easypb_schema::FILE_SYNTAX_UNSUPPORTED);
    CHECK(easypb_schema::classify_file_syntax(from_syntax) ==
          easypb_schema::FILE_SYNTAX_UNSUPPORTED);
    CHECK(!easypb_schema::same_descriptor_file(from_edition, from_syntax));
    CHECK(!easypb_schema::same_descriptor_file(from_syntax, from_edition));

    // The control: two copies that both carry the edition bit are one file.
    const FileDescriptorProto same_edition = from_edition;
    CHECK(easypb_schema::same_descriptor_file(from_edition, same_edition));
}

// D2, the custom half: `(easypb.cpp).type` strings are interned like every
// other string, so generation from the loaded copy equals generation from a
// fresh ingestion of the same bytes. The fixture also carries a dependency
// (an import name), a
// proto3 syntax string, nested message and enum names, and type_name
// references -- every string class the adapter's walk must cover.
void test_loader_custom_type_strings_survive_the_container(
    const std::string& fixture_dir)
{
    const std::string path =
        fixture_dir + "/codegen/custom_types/data/custom_types.pbs";
    std::vector<std::string> inputs(1, path);
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 1);
    if (set.files().size() != 1) return;
    const easypb_schema::SchemaFile& file = *set.files()[0];
    CHECK(file.from_descriptor_set);
    CHECK(text(file.file.name) == "custom_types.proto");
    CHECK(text(file.file.package) == "easypb.test");
    CHECK(text(file.file.syntax) == "proto3");
    CHECK(file.file.dependency.size() == 1);
    if (file.file.dependency.size() == 1) {
        CHECK(text(file.file.dependency[0]) == "easypb/options.proto");
    }
    CHECK(file.file.message_type.size() == 1);
    if (file.file.message_type.size() == 1) {
        const DescriptorProto& message = file.file.message_type[0];
        CHECK(text(message.name) == "CustomTypes");
        CHECK(text(message.nested_type[0].name) == "Item");
        CHECK(text(message.nested_type[1].name) == "OrdinaryItemsEntry");
        CHECK(text(message.enum_type[0].name) == "Kind");
        CHECK(text(message.enum_type[0].value[0].name) == "ZERO");
        CHECK(text(message.field[4].type_name) ==
              ".easypb.test.CustomTypes.Item");
    }
    check_cpp_type(file, "CustomTypes", "custom_name", "FixedString<64>");
    check_cpp_type(file, "CustomTypes", "item", "MessageBox<{}>");
    check_cpp_type(file, "CustomTypes", "aliases", "SmallVector<{}, 4>");
    check_cpp_type(file, "CustomTypes", "items", "std::deque<{}>");
    check_cpp_type(file, "CustomTypes", "items_by_name",
                   "std::unordered_map<{}, {}>");
    check_cpp_type(file, "CustomTypes", "scalar", "WrappedInt");
    check_cpp_type(file, "CustomTypes", "kind", "EnumBox<{}>");
    check_cpp_type(file, "CustomTypes", "opaque_values", "OpaqueRepeated");
    check_cpp_type(file, "CustomTypes", "opaque_map", "OpaqueMap");

    // Indexing: one descriptor-origin file is exactly the compatibility
    // mode's precondition; the absent `easypb/options.proto` target warns
    // nothing (no external reference) but keeps `complete` false.
    easypb_schema::LinkedSchema linked;
    CHECK(easypb_schema::link_schema(
        set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
        error));
    CHECK(linked.files == &set);
    CHECK(!linked.local_only);
    CHECK(file.warnings.empty());

    // Generation, live vs owned: the custom template is read from the option,
    // so a missed interning would surface as a byte difference here. The
    // linked entry point derives its provenance from the logical name, so no
    // provenance argument is passed any more.
    const std::string from_owned = generate_linked_one(file, linked);
    CHECK(from_owned.find("std::deque") != std::string::npos);
    {
        easypb_schema::SchemaSet live_set;
        std::vector<std::string> live_inputs(1, path);
        CHECK(easypb_schema::load_descriptor_sets(live_inputs, live_set, error));
        easypb_schema::LinkedSchema live_linked;
        CHECK(easypb_schema::link_schema(
            live_set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY,
            live_linked, error));
        easypb_schema::SchemaFile* live_target =
            live_set.find_file("custom_types.proto");
        CHECK(live_target != 0);
        if (live_target != 0) {
            CHECK(from_owned == generate_linked_one(*live_target, live_linked));
        }
    }
}

// V-14: `public_dependency` and `weak_dependency` are indexes into
// `dependency`, and the index lists are sets: the storage order carries no
// meaning. Two containers of one logical file may spell the same index set in
// different orders; the merge must accept both orders and keep the first
// copy's stored order, because the dependency includes and the print dump
// follow that order. A different index is a real difference and stays a
// conflict.
void test_identity_compares_dependency_indexes_as_multisets()
{
    const std::string ordered = dependency_merge_set(false, false);
    const std::string reversed = dependency_merge_set(true, false);
    const FileDescriptorSet a = easypb::decode<FileDescriptorSet>(ordered);
    const FileDescriptorSet b = easypb::decode<FileDescriptorSet>(reversed);
    CHECK(a.file.size() == 1);
    CHECK(b.file.size() == 1);
    CHECK(easypb_schema::same_descriptor_file(a.file[0], b.file[0]));
    CHECK(easypb_schema::same_descriptor_file(b.file[0], a.file[0]));

    // The comparator never rewrites the model it reads.
    CHECK(a.file[0].public_dependency[0] == 0);
    CHECK(a.file[0].public_dependency[1] == 2);
    CHECK(b.file[0].public_dependency[0] == 2);
    CHECK(b.file[0].public_dependency[1] == 0);

    const std::string different = dependency_merge_set(false, true);
    const FileDescriptorSet c = easypb::decode<FileDescriptorSet>(different);
    CHECK(!easypb_schema::same_descriptor_file(a.file[0], c.file[0]));
    CHECK(!easypb_schema::same_descriptor_file(c.file[0], a.file[0]));

    // A multiset, not a set: a repeated index is a malformed list the linker
    // refuses, and two lists that disagree about multiplicity are not one file.
    FileDescriptorProto duplicate = a.file[0];
    duplicate.public_dependency[1] = 0;
    CHECK(!easypb_schema::same_descriptor_file(a.file[0], duplicate));
}

// The merge path, not only the comparator: two containers whose index lists
// disagree only in order load as one file, and a genuinely different weak index
// is still refused in both argument orders.
void test_loader_merges_dependency_index_order()
{
    const std::string first = "v14-ordered.pbs";
    const std::string second = "v14-reversed.pbs";
    const std::string different = "v14-different.pbs";
    CHECK(write_binary_file(first, dependency_merge_set(false, false)));
    CHECK(write_binary_file(second, dependency_merge_set(true, false)));
    CHECK(write_binary_file(different, dependency_merge_set(false, true)));

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    std::vector<std::string> inputs;
    inputs.push_back(first);
    inputs.push_back(second);
    CHECK(easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(set.files().size() == 1);
    CHECK(set.find_file("deps-merge.proto") != 0);
    // The first container is the copy the set keeps, with its stored order.
    if (set.files().size() == 1) {
        const std::vector<std::int32_t>& indexes =
            set.files()[0]->file.public_dependency;
        CHECK(indexes.size() == 2);
        if (indexes.size() == 2) {
            CHECK(indexes[0] == 0);
            CHECK(indexes[1] == 2);
        }
    }

    for (int order = 0; order < 2; ++order) {
        inputs.clear();
        inputs.push_back(order == 0 ? first : different);
        inputs.push_back(order == 0 ? different : first);
        CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("\"deps-merge.proto\"") != std::string::npos);
        CHECK(set.files().empty());
    }
    std::remove(first.c_str());
    std::remove(second.c_str());
    std::remove(different.c_str());
}

// V-18/V-35: the adapter's standalone name rule is the publisher's rule plus
// the control-character class, so the two input doors cannot disagree about a
// name. `../escape.proto` and DEL are refused; a valid relative name is
// accepted.
void test_adapter_name_rule_refuses_dotdot_and_del()
{
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::validate_descriptor_file_name("../escape.proto",
                                                        "c.pbs", error));
    CHECK(error.file == "c.pbs");
    CHECK(error.message.find("'..' components") != std::string::npos);

    std::string del_name = "name";
    del_name.push_back('\x7f');
    del_name += ".proto";
    CHECK(!easypb_schema::validate_descriptor_file_name(del_name, "c.pbs",
                                                        error));
    CHECK(error.message.find("control character") != std::string::npos);
    CHECK(error.message.find("0x7f") != std::string::npos);

    std::string valid = "dir/name.proto";
    error = easypb_schema::Diagnostic();
    CHECK(easypb_schema::validate_descriptor_file_name(valid, "c.pbs", error));
    CHECK(error.message.empty());
}

// V-42, the chosen contract: a label outside the three descriptor.proto
// enumerators is refused at ingestion, with a diagnostic that names the field,
// its message scope and the value. protoc's decoder routes the value to the
// unknown fields instead, so this is a deliberate strictness: the model must
// not carry a value the emitter ignores but the merge would compare.
void test_adapter_rejects_an_unknown_label()
{
    const std::string bytes = label_set(99, true);
    const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(bytes);

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::append_descriptor_files(decoded.file, "label-99.pbs",
                                                  set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.file == "label-99.pbs");
    CHECK(error.message.find("field \"a\"") != std::string::npos);
    CHECK(error.message.find(".p.M") != std::string::npos);
    CHECK(error.message.find("label 99") != std::string::npos);
    CHECK(set.files().empty());

    // The container path refuses the same bytes with the container as the
    // diagnostic's file.
    const std::string path = "v42-label-99.pbs";
    CHECK(write_binary_file(path, bytes));
    std::vector<std::string> inputs(1, path);
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.file == path);
    CHECK(error.message.find("label 99") != std::string::npos);
    CHECK(set.files().empty());

    // A failure on a later container clears the files an earlier container
    // already appended: the loader's all-or-nothing contract.
    const std::string valid_path = "v42-label-valid.pbs";
    CHECK(write_binary_file(valid_path, label_set(1, true)));
    inputs.clear();
    inputs.push_back(valid_path);
    inputs.push_back(path);
    CHECK(!easypb_schema::load_descriptor_sets(inputs, set, error));
    CHECK(error.file == path);
    CHECK(set.files().empty());
    std::remove(valid_path.c_str());
    std::remove(path.c_str());

    // The control: an explicit LABEL_OPTIONAL and a protoc-shaped absent label
    // are accepted, and two identical copies still merge as one file.
    const std::string optional_bytes = label_set(1, true);
    const FileDescriptorSet explicit_optional =
        easypb::decode<FileDescriptorSet>(optional_bytes);
    CHECK(easypb_schema::append_descriptor_files(explicit_optional.file,
                                                 "label-ok.pbs", set, error));
    CHECK(set.files().size() == 1);
    const std::string same = "v42-identical.pbs";
    CHECK(write_binary_file(same, label_set(1, true)));
    std::vector<std::string> twice(2, same);
    CHECK(easypb_schema::load_descriptor_sets(twice, set, error));
    CHECK(set.files().size() == 1);
    const std::string absent_bytes = label_set(0, false);
    const FileDescriptorSet absent =
        easypb::decode<FileDescriptorSet>(absent_bytes);
    easypb_schema::SchemaSet absent_set;
    CHECK(easypb_schema::append_descriptor_files(absent.file, "label-absent.pbs",
                                                 absent_set, error));
    CHECK(absent_set.files().size() == 1);
    std::remove(same.c_str());
}

// V-42, the merge half: the identity reads the EFFECTIVE label. protoc's
// decoder routes an unknown enumerator to the unknown fields, so a
// protoc-written copy carries no label and a crafted raw 99 is the same
// effective LABEL_OPTIONAL. Ingestion refuses the crafted copy before the merge
// (the test above); this pins the comparator's own rule for a library caller
// that compares raw descriptors, so a raw unknown value can never decide
// identity.
void test_identity_reads_an_unknown_label_as_optional()
{
    FileDescriptorProto protoc_written;
    protoc_written.name = "label-merge.proto";
    protoc_written.package = "p";
    DescriptorProto message;
    message.name = "M";
    FieldDescriptorProto field;
    field.name = "a";
    field.number = 1;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_INT32;
    field.has_type = true;
    message.field.push_back(field);
    protoc_written.message_type.push_back(message);

    FileDescriptorProto crafted = protoc_written;
    crafted.message_type[0].field[0].label = 99;

    CHECK(easypb_schema::same_descriptor_file(protoc_written, crafted));
    CHECK(easypb_schema::same_descriptor_file(crafted, protoc_written));
    const FileDescriptorProto identical = crafted;
    CHECK(easypb_schema::same_descriptor_file(crafted, identical));

    // A declared label is still modeled content: required is another file.
    FileDescriptorProto required = protoc_written;
    required.message_type[0].field[0].label =
        FieldDescriptorProto::LABEL_REQUIRED;
    CHECK(!easypb_schema::same_descriptor_file(protoc_written, required));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1][0] == '\0') {
        std::cerr << "usage: test_descriptor_sets <tests-source-dir>\n"
                  << "The argument is the tests/ source directory: the tracked\n"
                  << ".pbs fixtures live under codegen/*/data/. Run via ctest.\n";
        return EXIT_FAILURE;
    }
    const std::string fixture_dir = argv[1];
    {
        // Fail loudly on a wrong argument instead of letting every corpus row
        // read a bogus path.
        std::string probe;
        if (!read_binary_file(
                fixture_dir + "/codegen/custom_types/data/custom_types.pbs",
                probe)) {
            std::cerr << "fixture directory lacks "
                         "codegen/custom_types/data/custom_types.pbs: "
                      << fixture_dir << '\n';
            return EXIT_FAILURE;
        }
    }

    test_adapter_rejects_an_empty_input();
    test_adapter_rejects_an_unnamed_file();
    test_adapter_rejects_duplicate_names_within_and_across_calls();
    test_adapter_owns_a_decoded_buffer_and_generates_after_destruction(
        fixture_dir);
    test_adapter_owns_json_name(fixture_dir);
    test_adapter_owns_reserved_names(fixture_dir);
    test_loader_loads_a_complete_container(fixture_dir);
    test_loader_rejects_an_empty_container(fixture_dir);
    test_loader_rejects_an_unnamed_file_in_a_multi_file_container();
    test_loader_anonymous_fixture_keeps_no_identity(fixture_dir);
    test_loader_single_anonymous_file_is_not_addressable();
    test_loader_rejects_duplicate_names_within_one_container();
    test_loader_merges_duplicate_names_across_containers(fixture_dir);
    test_loader_merges_semantically_equal_copies();
    test_identity_compares_the_edition_carrier();
    test_identity_compares_dependency_indexes_as_multisets();
    test_loader_merges_dependency_index_order();
    test_adapter_name_rule_refuses_dotdot_and_del();
    test_adapter_rejects_an_unknown_label();
    test_identity_reads_an_unknown_label_as_optional();
    test_loader_custom_type_strings_survive_the_container(fixture_dir);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all descriptor ingestion tests passed\n";
    return EXIT_SUCCESS;
}
