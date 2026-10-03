#include "descriptor_loader.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "descriptor_adapter.hpp"
#include "file_paths.hpp"

namespace easypb_schema {
namespace {

bool fail(const std::string& file, const std::string& message,
          SchemaSet& result, Diagnostic& error)
{
    result.clear();
    error = Diagnostic();
    error.file = file;
    error.location = unknown_location();
    error.message = message;
    return false;
}

std::string count_text(std::size_t value)
{
    return easypb::detail::number_to_string(static_cast<uint64_t>(value));
}

// One file with no logical name: the legacy single-file `.pbs` workflow. The
// container path is an internal identity - it keys the set and labels
// diagnostics - and it is never written into the descriptor. The borrowed
// copy below only has to stay alive for the adapter call.
bool append_anonymous_descriptor(const FileDescriptorProto& file,
                                 const std::string& container_path,
                                 SchemaSet& result, Diagnostic& error)
{
    std::vector<FileDescriptorProto> borrowed(1, file);
    borrowed[0].name = container_path;
    if (!append_descriptor_files(borrowed, container_path, result, error)) {
        return false;
    }
    SchemaFile* slot = result.find_file(container_path);
    if (slot == 0) {
        // Unreachable while the adapter keys by the name it was given; kept
        // so a future change cannot silently leave the identity in the
        // descriptor.
        return fail(container_path,
                    container_path +
                        ": internal identity lost during descriptor ingestion",
                    result, error);
    }
    slot->file.name = str_view();
    slot->file.has_name = false;
    return true;
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
            decoded = easypb::decode<FileDescriptorSet>(bytes);
        } catch (const std::exception& problem) {
            return fail(path, path + ": cannot decode descriptor set: " +
                                    problem.what(),
                        result, error);
        }
        if (decoded.file.empty()) {
            return fail(path,
                        path + ": descriptor set contains no FileDescriptorProto",
                        result, error);
        }

        std::size_t unnamed = 0;
        for (std::size_t j = 0; j < decoded.file.size(); ++j) {
            if (decoded.file[j].name.empty()) ++unnamed;
        }
        if (unnamed != 0 && decoded.file.size() != 1) {
            return fail(path,
                        path + ": descriptor set holds " +
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
        } else if (!append_descriptor_files(decoded.file, path, result, error)) {
            return false;
        }

        // The wrapper is the only layer that knows the container; retain it
        // as metadata for the files this call just appended.
        for (std::size_t j = before; j < result.files().size(); ++j) {
            result.files()[j]->physical_name = path;
        }
    }
    return true;
}

} // namespace easypb_schema
