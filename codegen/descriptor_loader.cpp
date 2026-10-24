#include "descriptor_loader.hpp"

#include <cstddef>
#include <exception>
#include <set>
#include <string>
#include <vector>

#include "descriptor_adapter.hpp"
#include "file_paths.hpp"

namespace easypb_schema {
namespace {

// The three field numbers the nesting walk needs (descriptor.proto, and the
// same numbers codegen/descriptor.pb.hpp decodes): a FileDescriptorSet holds
// its files in field 1, a FileDescriptorProto holds its messages in field 4,
// and a DescriptorProto holds its nested messages in field 3.
enum {
    FILE_DESCRIPTOR_SET_FILE = 1,
    FILE_DESCRIPTOR_PROTO_MESSAGE_TYPE = 4,
    DESCRIPTOR_PROTO_NESTED_TYPE = 3
};

// The messages nested in one DescriptorProto, and the depth of the deepest of
// them. `depth` is the depth of the message the reader is walking.
//
// The walk descends only into the nested messages and stops at the limit, so
// its own recursion never exceeds the limit it enforces.
int measure_message_nesting(easypb::Reader& reader, int depth, int limit)
{
    int deepest = depth;
    while (reader.get_next_field()) {
        if (reader.field_num != DESCRIPTOR_PROTO_NESTED_TYPE ||
            reader.wire_type != easypb::WIRETYPE_LENGTH_DELIMITED) {
            reader.skip_field();
            continue;
        }
        const easypb::string_view nested = reader.parse_bytearray_value();
        const int child_depth = depth + 1;
        if (child_depth >= limit) return child_depth;
        easypb::Reader nested_reader(nested);
        const int child_deepest =
            measure_message_nesting(nested_reader, child_depth, limit);
        if (child_deepest > deepest) deepest = child_deepest;
        if (deepest >= limit) return deepest;
    }
    return deepest;
}

// The depth of the deepest message the bytes of a descriptor set declare, or 0
// when they declare none.
//
// The walk runs on the BYTES and not on a decoded tree, because the decode is
// the recursion that overflows the stack: the runtime's message codec
// descends into every nested message it reads, so a set that nests deeper
// than the limit has to be refused before anything is decoded. The limit is
// the reference compiler's own (descriptor.h:3045,
// MaxMessageDeclarationNestingDepth() == 32) and the sentence is the one it
// uses for the same shape ("Reached maximum recursion limit for nested
// messages.", compiler/parser.cc:785-789 and descriptor.cc:6364-6373).
//
// The walk visits only the fields the decoder reads. A nested message hidden
// in an unknown field is skipped by the decoder too, so it is not a nesting
// the tool ever follows.
int measure_set_nesting(const std::string& bytes)
{
    const int limit = max_message_nesting_depth();
    easypb::Reader set(bytes.data(), bytes.size());
    int deepest = 0;
    while (set.get_next_field()) {
        if (set.field_num != FILE_DESCRIPTOR_SET_FILE ||
            set.wire_type != easypb::WIRETYPE_LENGTH_DELIMITED) {
            set.skip_field();
            continue;
        }
        const easypb::string_view file = set.parse_bytearray_value();
        easypb::Reader file_reader(file);
        while (file_reader.get_next_field()) {
            if (file_reader.field_num != FILE_DESCRIPTOR_PROTO_MESSAGE_TYPE ||
                file_reader.wire_type != easypb::WIRETYPE_LENGTH_DELIMITED) {
                file_reader.skip_field();
                continue;
            }
            const easypb::string_view message =
                file_reader.parse_bytearray_value();
            easypb::Reader message_reader(message);
            const int depth = measure_message_nesting(message_reader, 1, limit);
            if (depth > deepest) deepest = depth;
            if (deepest >= limit) return deepest;
        }
    }
    return deepest;
}

bool fail(const std::string& file, const std::string& message,
          SchemaSet& result, Diagnostic& error,
          DiagnosticCode code = DIAGNOSTIC_GENERIC)
{
    result.clear();
    error = Diagnostic();
    error.file = file;
    error.location = unknown_location();
    error.message = message;
    error.code = code;
    return false;
}

std::string count_text(std::size_t value)
{
    return easypb::detail::number_to_string(static_cast<uint64_t>(value));
}

// One file with no logical name: the legacy single-file `.pbs` workflow. The
// container path is an internal identity - it labels diagnostics and is
// retained as physical_name - and it is never written into the descriptor or
// interned into its pool (W-57). append_unnamed_descriptor owns the whole
// ingestion shape, so no fabricated name has to be cleared afterwards.
bool append_anonymous_descriptor(const FileDescriptorProto& file,
                                 const std::string& container_path,
                                 SchemaSet& result, Diagnostic& error)
{
    return append_unnamed_descriptor(file, container_path, result, error);
}

} // namespace

bool load_descriptor_sets(const std::vector<std::string>& input_paths,
                          SchemaSet& result, Diagnostic& error)
{
    if (input_paths.empty()) {
        return fail(std::string(),
                    "load_descriptor_sets needs at least one descriptor set input",
                    result, error);
    }

    for (std::size_t i = 0; i < input_paths.size(); ++i) {
        const std::string& path = input_paths[i];
        std::string bytes;
        std::string read_error;
        if (!easypb_file::read_disk_file(path, bytes, read_error)) {
            return fail(path, read_error, result, error);
        }

        const std::size_t before = result.files().size();
        FileDescriptorSet decoded;
        try {
            // The nesting bound, before the decode. The decode descends into
            // every nested message the set carries, so a set that nests deeper
            // than the limit would overflow the stack inside the runtime's
            // message codec - before any of this project's code could look at
            // the bytes. The walk that measures the nesting runs on the bytes
            // and stops at the limit, so it cannot overflow the stack either.
            if (!check_descriptor_set_nesting(bytes, path, error)) {
                // The nesting check filled `error` with the container label;
                // its sentence and code are the diagnostic, and `fail`
                // supplies the label (W-19: the path prints once).
                const std::string message = error.message;
                const DiagnosticCode code = error.code;
                return fail(path, message, result, error, code);
            }
            decoded = easypb::decode<FileDescriptorSet>(bytes);
        } catch (const std::exception& problem) {
            return fail(path, std::string("cannot decode descriptor set: ") +
                                    problem.what(),
                        result, error);
        }
        if (decoded.file.empty()) {
            return fail(path,
                        "descriptor set contains no FileDescriptorProto",
                        result, error);
        }

        std::size_t unnamed = 0;
        for (std::size_t j = 0; j < decoded.file.size(); ++j) {
            if (decoded.file[j].name.empty()) ++unnamed;
        }
        if (unnamed != 0 && decoded.file.size() != 1) {
            return fail(path,
                        "descriptor set holds " +
                            count_text(decoded.file.size()) + " files and " +
                            count_text(unnamed) +
                            " of them have no logical name; only a single-file "
                            "set may omit it",
                        result, error);
        }

        if (unnamed == 1) {
            if (!append_anonymous_descriptor(decoded.file[0], path, result,
                                             error)) {
                return false;
            }
        } else {
            // `W-36`: a file several containers carry is one file, not a
            // collision. protoc merges the copies and keeps the first
            // (measured 2026-10-22, the G4b probe pair), and so does this:
            // an identical copy is dropped, and only two DIFFERENT
            // definitions of one logical name are refused, because silently
            // keeping one of them would generate a header from a schema the
            // caller may not have meant. The comparison is over the modeled
            // descriptor (same_descriptor_file), not over the two containers'
            // bytes: a re-encoded set and its source are the same file.
            std::vector<FileDescriptorProto> fresh;
            // Two copies of one name INSIDE one container stay refused: the
            // merge below is the cross-container rule (W-36), and a set that
            // names one file twice is malformed whoever else carries it.
            std::set<std::string> carried_names;
            for (std::size_t j = 0; j < decoded.file.size(); ++j) {
                const FileDescriptorProto& file = decoded.file[j];
                const std::string logical = view_text(file.name);
                if (!carried_names.insert(logical).second) {
                    return fail(
                        path,
                        "duplicate logical file name \"" + logical +
                            "\" in descriptor source \"" + path +
                            "\" (a file with this name was already appended)",
                        result, error, DIAGNOSTIC_DUPLICATE_SYMBOL);
                }
                SchemaFile* existing = result.find_file(logical);
                if (existing == 0) {
                    fresh.push_back(file);
                    continue;
                }
                if (same_descriptor_file(existing->file, file)) continue;
                return fail(
                    path,
                    "descriptor file \"" + logical +
                        "\" conflicts with the copy carried by \"" +
                        existing->physical_name +
                        "\": two containers carry different definitions of "
                        "one logical file",
                    result, error, DIAGNOSTIC_DUPLICATE_SYMBOL);
            }
            if (!fresh.empty() &&
                !append_descriptor_files(fresh, path, result, error)) {
                return false;
            }
        }

        // The wrapper is the only layer that knows the container; retain it
        // as metadata for the files this call just appended.
        for (std::size_t j = before; j < result.files().size(); ++j) {
            result.files()[j]->physical_name = path;
        }
    }
    return true;
}

bool check_descriptor_set_nesting(const std::string& bytes,
                                  const std::string& label,
                                  Diagnostic& error)
{
    const int limit = max_message_nesting_depth();
    int deepest = 0;
    try {
        deepest = measure_set_nesting(bytes);
    } catch (const std::exception&) {
        // The walk met bytes it cannot read. The decode meets the same bytes
        // and reports the same problem, so the set is not refused here: the
        // refusal belongs to the decode, which says what is wrong with it.
        return true;
    }
    if (deepest < limit) return true;
    error.file = label;
    error.location = unknown_location();
    error.message = "Reached maximum recursion limit for nested messages.";
    error.warning = false;
    error.code = DIAGNOSTIC_GENERIC;
    return false;
}

} // namespace easypb_schema
