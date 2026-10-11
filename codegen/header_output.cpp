// The standalone publisher: the one component that turns the generator's
// in-memory result into files on disk.
//
// Trimmed to the reference compiler's model on 2026-10-07. What is left is what
// protoc's WriteAllToDisk does (compiler/command_line_interface.cc:541-610):
// refuse two targets that map to one output name, verify the output directory,
// create the missing parent directories, and write each file. The name a
// GeneratedFile carries was already mapped and validated by
// easypb_codegen::generated_header_path (codegen/header_paths.cpp), which is
// where a descriptor set naming "../escape.proto" is refused - measured, that
// refusal holds without any of the guards removed here, because it is upstream
// of this module.
//
// What was removed, with its measured effect: containment below the output
// root (protoc writes one directory above when a descriptor set asks it to),
// case-fold collision probing (its ASCII-only folding already missed every
// non-ASCII pair, review CR-1 of 2026-10-05), native file identity and hardlink
// detection, and the atomic temp+rename replacement (protoc truncates in
// place).

#include "header_output.hpp"

#include <set>

#include "file_paths.hpp"
#include "logical_paths.hpp"
#include "schema.hpp"

namespace easypb_codegen {

namespace {

easypb_schema::Diagnostic make_error(const std::string& file,
                                    const std::string& message)
{
    easypb_schema::Diagnostic diagnostic;
    diagnostic.file = file;
    diagnostic.location = easypb_schema::unknown_location();
    diagnostic.message = message;
    diagnostic.code = easypb_schema::DIAGNOSTIC_GENERIC;
    return diagnostic;
}

} // namespace

// The parent of an already-normalized absolute spelling. Never returns the
// path itself, so make_directories cannot be pointed at the output file.
std::string parent_of_absolute(const std::string& absolute)
{
    const std::size_t slash = absolute.rfind('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    std::string parent = absolute.substr(0, slash);
#ifdef _WIN32
    if (parent.size() == 2 && parent[1] == ':' &&
        ((parent[0] >= 'a' && parent[0] <= 'z') ||
         (parent[0] >= 'A' && parent[0] <= 'Z'))) {
        parent += "/";
    }
#endif
    return parent;
}

bool publish_headers(const std::string& out_dir,
                     const std::vector<GeneratedFile>& files,
                     easypb_schema::Diagnostic& error)
{
    error = easypb_schema::Diagnostic();

    if (out_dir.empty()) {
        error = make_error(std::string(),
                           "cannot publish headers without an output directory");
        return false;
    }
    if (files.empty()) {
        error = make_error(out_dir, "no generated headers to publish");
        return false;
    }

    // Two targets must not map to one output file. The reference compiler's
    // own context keeps its outputs in a map, so the second target silently
    // wins there; refusing the collision costs four lines and keeps the
    // outcome a diagnostic instead of a silently dropped header.
    std::set<std::string> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!names.insert(files[i].name).second) {
            error = make_error(files[i].name,
                               "two targets map to one output file \"" +
                               files[i].name + "\"");
            return false;
        }
    }

    // The output directory itself must exist, and a file at that spelling is
    // the classic denied output root rather than a usable directory.
    std::string directory_error;
    if (!easypb_file::make_directories(out_dir, directory_error)) {
        error = make_error(out_dir, directory_error);
        return false;
    }

    // One file at a time. The parent directories are created first, so a
    // header nested below the output root does not need the directory to have
    // been made by hand - the reference compiler's TryCreateParentDirectory
    // does the same from the relative name's components
    // (command_line_interface.cc:181-201).
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string target =
            easypb_file::lexical_absolute(
                easypb_file::join_disk_path(out_dir, files[i].name));
        if (target.empty()) {
            error = make_error(files[i].name,
                               files[i].name + ": cannot resolve the output path");
            return false;
        }
        const std::string parent = parent_of_absolute(target);
        if (!parent.empty()) {
            std::string parent_error;
            if (!easypb_file::make_directories(parent, parent_error)) {
                error = make_error(target, parent_error);
                return false;
            }
        }
        std::string write_error;
        if (!easypb_file::write_disk_file(target, files[i].content,
                                         write_error)) {
            error = make_error(target, write_error);
            return false;
        }
    }
    return true;
}

} // namespace easypb_codegen
