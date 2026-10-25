// The standalone publisher: the one component that turns the generator's
// in-memory result into files on disk.
//
// Trimmed to the reference compiler's model by the plan-5 trim (`fbdcc0e`,
// 2026-10-11). What is left is what protoc's WriteAllToDisk does
// (compiler/command_line_interface.cc:541-610): refuse two targets that map to
// one output name, create the output directory, create the missing parent
// directories, and write each file in place. The name a
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
//
// What was added back, by owner decision of 2026-10-21: the publisher
// validates every output name as a logical path itself. The walk-based
// containment guard is still gone (a symlink below the output root is
// followed, as it is in protoc), but a name that escapes the root by its own
// spelling - "../escape.pb.hpp" - is refused with a diagnostic instead of
// being written one directory above the root. protoc writes it; this project
// refuses it, and the refusal happens before anything is written.

#include "header_output.hpp"

#include <map>
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

// The parent of an already-normalized absolute spelling. Never returns the
// path itself, so make_directories cannot be pointed at the output file.
// File-local by use (W-52): the publisher is its only caller, so it keeps
// internal linkage instead of publishing a declaration no header needs.
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

} // namespace

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

    // Every name the publisher is handed is a logical path (the contract
    // codegen/header_paths.hpp states), and this is the last door before a
    // name becomes a file: a name that escapes the output root
    // ("../escape.pb.hpp") is refused here, with a diagnostic, instead of
    // being written one directory above the root. The mapping layer
    // (generated_header_path) refuses the same name one layer up, so this
    // closes the hole a hand-built GeneratedFile would otherwise have.
    //
    // The reference compiler does not refuse it: protoc writes one directory
    // above --cpp_out when a descriptor set asks it to (measured against
    // protoc 30.1, build/plan5-code-review-1021-summary/verify/G2.md). This
    // is therefore a deliberate divergence, the owner's decision of
    // 2026-10-21.
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string reason;
        if (!easypb_schema::validate_logical_path(files[i].name, reason)) {
            error = make_error(files[i].name,
                               "cannot be published: " + reason);
            return false;
        }
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

    // The output directory itself is created when missing, and a file at that
    // spelling is the classic denied output root rather than a usable
    // directory. When a file sits in the middle of the path, the reason is
    // about that component: label the diagnostic with the component that
    // answered instead of claiming the requested path is a file (CR-40).
    std::string directory_error;
    std::string blocking_path;
    if (!easypb_file::make_directories(out_dir, directory_error,
                                       &blocking_path)) {
        error = make_error(blocking_path.empty() ? out_dir : blocking_path,
                           directory_error);
        return false;
    }

    // One file at a time. The parent directories are created first, so a
    // header nested below the output root does not need the directory to have
    // been made by hand - the reference compiler's TryCreateParentDirectory
    // does the same from the relative name's components
    // (command_line_interface.cc:181-201).
    //
    // The files are written in byte-wise name order. The reference compiler
    // keeps its outputs in a map keyed by output name and writes them in that
    // order (command_line_interface.cc:541-610), so when two output names are
    // one file on a case-folding filesystem the survivor is the byte-wise
    // greater spelling whatever order the targets were named in. Reproducing
    // the order makes this publisher agree with that rule; case aliases stay
    // accepted (the case-fold probing was removed on purpose, trim `fbdcc0e`,
    // ledger W-27) - only the survivor stops depending on --target order
    // (S4/W-26). Exact duplicates were refused above, so the map is one entry
    // per file.
    std::map<std::string, const GeneratedFile*> ordered;
    for (std::size_t i = 0; i < files.size(); ++i) {
        ordered[files[i].name] = &files[i];
    }

    for (std::map<std::string, const GeneratedFile*>::const_iterator entry =
             ordered.begin();
         entry != ordered.end(); ++entry) {
        const GeneratedFile& file = *entry->second;
        const std::string target =
            easypb_file::lexical_absolute(
                easypb_file::join_disk_path(out_dir, file.name));
        if (target.empty()) {
            error = make_error(
                file.name,
                "cannot resolve the output path (the current working "
                "directory is unknown)");
            return false;
        }
        const std::string parent = parent_of_absolute(target);
        if (!parent.empty()) {
            std::string parent_error;
            std::string blocking_path;
            if (!easypb_file::make_directories(parent, parent_error,
                                               &blocking_path)) {
                // The failure belongs to the component that answered, not to
                // the header below it; the reason names no path, so the
                // diagnostic supplies it once (W-29, CR-40).
                error = make_error(
                    blocking_path.empty() ? parent : blocking_path,
                    parent_error);
                return false;
            }
        }
        std::string write_error;
        if (!easypb_file::write_disk_file(target, file.content,
                                         write_error)) {
            error = make_error(target, write_error);
            return false;
        }
    }
    return true;
}

} // namespace easypb_codegen
