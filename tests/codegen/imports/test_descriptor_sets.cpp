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

// D5: a container holding exactly one unnamed file keeps the legacy stdout
// compatibility path. The container path is an internal identity that labels
// diagnostics and is never written into the copied descriptor; it is not a
// logical name, so after the ingest it is not addressable through the set
// (S4/W-18: `--target <container>` must not select the unnamed file).
void test_loader_single_anonymous_file_is_not_addressable()
{
    const std::string scratch = "imports-descriptor-anonymous.pbs";
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
    CHECK(error.message.find(conflicting) != std::string::npos);
    CHECK(set.files().empty());
    CHECK(set.targets().empty());
    std::remove(conflicting.c_str());
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
    test_loader_loads_a_complete_container(fixture_dir);
    test_loader_rejects_an_empty_container(fixture_dir);
    test_loader_rejects_an_unnamed_file_in_a_multi_file_container();
    test_loader_single_anonymous_file_is_not_addressable();
    test_loader_rejects_duplicate_names_within_one_container();
    test_loader_merges_duplicate_names_across_containers(fixture_dir);
    test_loader_custom_type_strings_survive_the_container(fixture_dir);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all descriptor ingestion tests passed\n";
    return EXIT_SUCCESS;
}
