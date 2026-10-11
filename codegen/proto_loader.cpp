#include "proto_loader.hpp"

#include "file_paths.hpp"
#include "logical_paths.hpp"
#include "proto_parser.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace easypb_proto {
namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

// Joins already-quoted list items (search roots) and import chains
// alike: every element is wrapped in double quotes.
std::string join_quoted(const std::vector<std::string>& items,
                        const char* separator)
{
    std::string result;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) result += separator;
        result += "\"";
        result += items[i];
        result += "\"";
    }
    return result;
}

struct LoaderState {
    std::vector<std::string> search_roots;
    std::vector<std::string> search_roots_absolute;
    easypb_schema::SchemaSet* result;
    // Discovery path from the selected root to the file being loaded,
    // used only to spell diagnostics.
    std::vector<std::string> chain;

    LoaderState() : result(0) {}
};

std::string chain_text(const std::vector<std::string>& chain,
                       const std::string& leaf)
{
    std::vector<std::string> full = chain;
    if (!leaf.empty()) full.push_back(leaf);
    if (full.empty()) return std::string();
    return join_quoted(full, " -> ");
}

// Resolve a logical import through ordered search roots. Returns true
// with the winning disk path; the first matching root wins and is never
// substituted based on the importing file's location. A root that cannot
// be reached stops the search with its reason in access_error: only
// confirmed absence may advance to the next root, so a denied access can
// never silently select another file.
bool resolve_logical(const std::string& logical,
                     const std::vector<std::string>& search_roots,
                     std::string& disk_path,
                     std::string* access_error)
{
    if (access_error != 0) access_error->clear();
    for (std::size_t i = 0; i < search_roots.size(); ++i) {
        const std::string candidate =
            easypb_file::join_disk_path(search_roots[i], logical);
        std::string status_error;
        const easypb_file::FileStatus status =
            easypb_file::disk_file_status(candidate, status_error);
        if (status == easypb_file::FILE_STATUS_ERROR) {
            if (access_error != 0) *access_error = status_error;
            return false;
        }
        // Only a regular file is a candidate. A directory (or any other
        // non-file) never matches and never stops the search, exactly
        // like protoc's OpenVirtualFile when OpenDiskFile fails on one.
        if (status != easypb_file::FILE_STATUS_FILE) continue;
        disk_path = candidate;
        return true;
    }
    return false;
}

bool fail(easypb_schema::Diagnostic& error,
          const std::string& file,
          const easypb_schema::SourceLocation& location,
          easypb_schema::DiagnosticCode code,
          const std::string& message)
{
    error.file = file;
    error.location = location;
    error.message = message;
    error.warning = false;
    error.code = code;
    return false;
}

// Source position of a dependency declaration: the index-aligned imports
// entry when it names the same path, otherwise the first entry with a
// matching path, otherwise unknown. Imports supply positions only; the
// descriptor lists stay authoritative for graph meaning.
easypb_schema::SourceLocation import_location(
    const easypb_schema::SchemaFile& slot, std::size_t index,
    const std::string& dependency)
{
    easypb_schema::SourceLocation where =
        easypb_schema::unknown_location();
    if (index < slot.imports.size() &&
        slot.imports[index].path == dependency) {
        return slot.imports[index].location;
    }
    for (std::size_t k = 0; k < slot.imports.size(); ++k) {
        if (slot.imports[k].path == dependency) {
            where = slot.imports[k].location;
            break;
        }
    }
    return where;
}

// Outcome of mapping an existing disk path to a logical name.
enum MapOutcome {
    MAP_FOUND,   // logical name and its resolved disk path are set
    MAP_OUTSIDE, // the path lies under no search root
    MAP_FAILED   // a diagnostic was stored in error
};

// Map an existing disk path to the logical name it has under the first
// search root that contains it, then verify the mapping against search
// precedence: when the ordered roots resolve that logical name to a
// different file, the input is shadowed by an earlier root and the load
// stops instead of compiling a different file. The reference compiler
// performs the same disk-to-name mapping (DiskFileToVirtualFile) and
// reports the same state; here the comparison is textual, so it needs no
// identity query.
MapOutcome map_disk_to_logical(const std::string& argument,
                               const LoaderState& state,
                               std::string& logical,
                               std::string& resolved_disk,
                               easypb_schema::Diagnostic& error)
{
    const std::string file_absolute = easypb_file::lexical_absolute(argument);
    // A file physically under a search root whose remainder is not a
    // valid logical name must not fall through to the "outside" answer:
    // the first such root is remembered while narrower later roots may
    // still yield a valid mapping.
    bool saw_invalid_candidate = false;
    std::string invalid_candidate;
    std::string invalid_root;
    std::string invalid_reason;
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        const std::string& root_absolute = state.search_roots_absolute[i];
        std::string candidate;
        if (!easypb_file::strip_search_root_prefix(file_absolute,
                                                   root_absolute,
                                                   candidate)) {
            continue;
        }
        std::string reason;
        if (!easypb_schema::validate_logical_path(candidate, reason)) {
            if (!saw_invalid_candidate) {
                saw_invalid_candidate = true;
                invalid_candidate = candidate;
                invalid_root = state.search_roots[i];
                invalid_reason = reason;
            }
            continue;
        }

        std::string winner_disk;
        std::string access_error;
        if (!resolve_logical(candidate, state.search_roots, winner_disk,
                             &access_error)) {
            if (access_error.empty()) continue;
            error.file = argument;
            error.location = easypb_schema::unknown_location();
            error.message = access_error;
            error.warning = false;
            error.code = easypb_schema::DIAGNOSTIC_GENERIC;
            return MAP_FAILED;
        }
        if (easypb_file::lexical_absolute(winner_disk) != file_absolute) {
            std::ostringstream message;
            message << "root \"" << argument << "\" maps to \"" << candidate
                    << "\" but search precedence resolves \"" << candidate
                    << "\" to \"" << winner_disk
                    << "\" (shadowed by an earlier search root)";
            error.file = argument;
            error.location = easypb_schema::unknown_location();
            error.message = message.str();
            error.warning = false;
            error.code = easypb_schema::DIAGNOSTIC_INVALID_IMPORT;
            return MAP_FAILED;
        }
        logical = candidate;
        resolved_disk = winner_disk;
        return MAP_FOUND;
    }
    if (saw_invalid_candidate) {
        std::ostringstream message;
        message << "root \"" << argument << "\" lies under search root \""
                << invalid_root << "\" but maps to invalid logical name \""
                << invalid_candidate << "\": " << invalid_reason;
        error.file = argument;
        error.location = easypb_schema::unknown_location();
        error.message = message.str();
        error.warning = false;
        error.code = easypb_schema::DIAGNOSTIC_INVALID_IMPORT;
        return MAP_FAILED;
    }
    return MAP_OUTSIDE;
}

bool load_recursive(const std::string& logical,
                    const std::string& disk_path,
                    LoaderState& state,
                    easypb_schema::Diagnostic& error);

// Reads one disk file and fills its reserved slot with deferred parsing.
// The temporary source buffer is local to this function, so its lifetime
// verifiably ends on return, before the caller follows imports: in C++11
// shrink_to_fit is only a non-binding request and cannot guarantee that
// ancestor buffers are released during a deep import chain.
bool read_and_parse_source(const std::string& logical,
                           const std::string& disk_path,
                           const std::vector<std::string>& chain,
                           easypb_schema::SchemaFile& slot,
                           easypb_schema::Diagnostic& error)
{
    std::string contents;
    {
        std::string read_error;
        if (!easypb_file::read_disk_file(disk_path, contents, read_error)) {
            std::ostringstream message;
            message << "file \"" << logical << "\": " << read_error;
            if (!chain.empty()) {
                message << " (chain " << chain_text(chain, logical) << ")";
            }
            return fail(error, logical,
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_GENERIC, message.str());
        }
    }

    easypb_schema::Diagnostic parse_error;
    easypb_proto::ParseOptions options;
    options.defer_type_resolution = true;
    // Deferred parsing copies every retained string into the file's
    // pool, so nothing below borrows from the temporary buffer.
    const bool ok = easypb_proto::parse_proto(
        logical, contents.data(), contents.size(), slot, parse_error,
        options);
    // The parser resets physical_name to the logical name; retain
    // the physical disk path for diagnostics.
    slot.physical_name = disk_path;
    if (!ok) {
        std::ostringstream message;
        message << "while loading \"" << logical << "\"";
        if (!chain.empty()) {
            message << " (chain " << chain_text(chain, logical) << ")";
        }
        message << ": ";
        if (!parse_error.file.empty()) message << parse_error.file;
        if (parse_error.location.line != 0) {
            message << ":" << parse_error.location.line << ":"
                    << parse_error.location.column;
        }
        if (!parse_error.message.empty()) {
            message << ": " << parse_error.message;
        }
        return fail(error, parse_error.file.empty()
                                       ? logical
                                       : parse_error.file,
                    parse_error.location, parse_error.code,
                    message.str());
    }
    return true;
}

bool load_dependencies(easypb_schema::SchemaFile& slot,
                       LoaderState& state,
                       easypb_schema::Diagnostic& error)
{
    const std::string importer = view_text(slot.file.name);
    const std::size_t count = slot.file.dependency.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::string dependency = view_text(slot.file.dependency[i]);
        const easypb_schema::SourceLocation where =
            import_location(slot, i, dependency);
        // The descriptor lists are authoritative for graph meaning;
        // imports supplies positions only, so the weak/public modifiers
        // come from weak_dependency/public_dependency rather than the
        // positional imports entry. This matches the binder wording
        // ("missing public import") for all three modifiers.
        bool is_weak = false;
        bool is_public = false;
        for (std::size_t k = 0; k < slot.file.weak_dependency.size(); ++k) {
            if (slot.file.weak_dependency[k] ==
                static_cast<std::int32_t>(i)) {
                is_weak = true;
                break;
            }
        }
        for (std::size_t k = 0; k < slot.file.public_dependency.size(); ++k) {
            if (slot.file.public_dependency[k] ==
                static_cast<std::int32_t>(i)) {
                is_public = true;
                break;
            }
        }
        const char* modifier = "";
        if (is_weak) modifier = "weak ";
        else if (is_public) modifier = "public ";

        std::string reason;
        if (!easypb_schema::validate_logical_path(dependency, reason)) {
            std::ostringstream message;
            message << "file \"" << importer << "\": invalid import \""
                    << dependency << "\": " << reason;
            if (!state.chain.empty()) {
                message << " (chain "
                        << chain_text(state.chain, dependency) << ")";
            }
            return fail(error, importer, where,
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        message.str());
        }

        std::string child_disk;
        std::string access_error;
        if (!resolve_logical(dependency, state.search_roots, child_disk,
                             &access_error)) {
            if (!access_error.empty()) {
                std::ostringstream message;
                message << "file \"" << importer << "\": cannot resolve "
                        << modifier << "import \"" << dependency
                        << "\": " << access_error
                        << "; searched "
                        << join_quoted(state.search_roots, ", ")
                        << "; chain "
                        << chain_text(state.chain, dependency);
                return fail(error, importer, where,
                            easypb_schema::DIAGNOSTIC_GENERIC,
                            message.str());
            }
            std::ostringstream message;
            message << "file \"" << importer << "\": missing "
                    << modifier << "import \"" << dependency
                    << "\"; searched " << join_quoted(state.search_roots, ", ");
            message << "; add the directory containing \"" << dependency
                    << "\" with -I (--proto_path)";
            message << "; chain " << chain_text(state.chain, dependency);
            return fail(error, importer, where,
                        easypb_schema::DIAGNOSTIC_MISSING_IMPORT,
                        message.str());
        }

        if (!load_recursive(dependency, child_disk, state, error)) {
            return false;
        }
    }
    return true;
}

bool load_recursive(const std::string& logical,
                    const std::string& disk_path,
                    LoaderState& state,
                    easypb_schema::Diagnostic& error)
{
    // A logical name is loaded once: a repeated command-line root or a
    // diamond import returns the file already present, exactly like the
    // descriptor pool of the reference compiler keyed by file name. Two
    // different files under one logical name are not distinguished here;
    // the resulting duplicate definitions surface later, as in protoc.
    if (state.result->find_file(logical) != 0) return true;

    // Reserve the logical name before following imports so repeated and
    // cyclic references cannot cause repeated parsing or infinite reads.
    // The slot is empty until deferred parsing fills it below.
    easypb_schema::SchemaFile* slot = 0;
    try {
        slot = &state.result->add_file(logical);
    } catch (const std::exception& failure) {
        return fail(error, logical,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, failure.what());
    }
    slot->physical_name = disk_path;

    // Read and deferred-parse inside a helper whose local source buffer
    // is destroyed on return, before dependencies are followed below.
    // Later reads, metadata, and name lookups must remain valid after
    // pool and vector growth.
    if (!read_and_parse_source(logical, disk_path, state.chain, *slot,
                               error)) {
        return false;
    }

    state.chain.push_back(logical);
    const bool dependencies_ok = load_dependencies(*slot, state, error);
    state.chain.pop_back();
    return dependencies_ok;
}

// Turn one command-line root into a logical name plus its disk path. An
// argument that names an existing file is used through search-root
// mapping and must lie under a search root, exactly like the proto_path
// requirement of the reference compiler; otherwise the argument is a
// logical name resolved through the ordered roots. A denied access is a
// failure in both shapes, never a fall-through to the other one.

// The failure shared by every argument shape that lies under no search
// root. The reference compiler reports the same state as "File does not
// reside within any path specified using --proto_path (or -I)".
bool fail_outside_search_roots(const std::string& argument,
                               const LoaderState& state,
                               easypb_schema::Diagnostic& error)
{
    std::ostringstream message;
    message << "root \"" << argument
            << "\" lies outside all search roots ("
            << join_quoted(state.search_roots, ", ")
            << "); add its directory with -I (--proto_path) or pass "
               "its logical name";
    return fail(error, argument, easypb_schema::unknown_location(),
                easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
}

// True when an existing path lies under one of the search roots, using
// the same byte-wise root mapping as the rest of the loader.
bool lies_under_search_root(const std::string& path,
                            const LoaderState& state,
                            std::string* containing_root)
{
    const std::string absolute = easypb_file::lexical_absolute(path);
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        std::string remainder;
        if (easypb_file::strip_search_root_prefix(
                absolute,
                state.search_roots_absolute[i],
                remainder)) {
            if (containing_root != 0) {
                *containing_root = state.search_roots[i];
            }
            return true;
        }
    }
    return false;
}

bool resolve_root(const std::string& argument, LoaderState& state,
                  std::string& logical, std::string& disk_path,
                  easypb_schema::Diagnostic& error)
{
    if (argument.empty()) {
        // protoc refuses an empty argument outright ("it is not valid
        // input"); the outcome is the same here, but the message names
        // the actual problem instead of calling the spelling an empty
        // logical path. An empty *search root* entry stays meaningful:
        // it means the working directory, as in the reference compiler.
        return fail(error, argument, easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                    "input file name is empty; pass a .proto path or a "
                    "logical name");
    }
    std::string status_error;
    const easypb_file::FileStatus root_status =
        easypb_file::disk_file_status(argument, status_error);
    if (root_status == easypb_file::FILE_STATUS_ERROR) {
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_GENERIC, status_error);
    }
    if (root_status == easypb_file::FILE_STATUS_FILE) {
        std::string mapped;
        std::string resolved;
        easypb_schema::Diagnostic mapping_error;
        switch (map_disk_to_logical(argument, state, mapped, resolved,
                                    mapping_error)) {
        case MAP_FOUND:
            logical = mapped;
            disk_path = resolved;
            return true;
        case MAP_FAILED:
            error = mapping_error;
            return false;
        case MAP_OUTSIDE:
            break;
        }
        return fail_outside_search_roots(argument, state, error);
    }
    if (root_status == easypb_file::FILE_STATUS_DIRECTORY) {
        // protoc maps a directory argument like any other path and then
        // fails to open it ("Input file is a directory"). Report the
        // same distinction here: under a root it is a directory error,
        // outside every root it stays the ordinary outside-roots
        // failure. A directory spelling is never retried as a logical
        // name, matching the reference compiler.
        std::string containing_root;
        if (lies_under_search_root(argument, state, &containing_root)) {
            std::ostringstream message;
            message << "root \"" << argument
                    << "\" is a directory, not a schema file (search root \""
                    << containing_root << "\")";
            return fail(error, argument,
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        message.str());
        }
        return fail_outside_search_roots(argument, state, error);
    }

    std::string reason;
    if (!easypb_schema::validate_logical_path(argument, reason)) {
        // A non-existent disk path that is also not a valid logical name
        // is reported as an invalid root spelling. protoc reports such an
        // argument as unresolvable instead; the outcome (a hard error, no
        // file loaded) is the same.
        std::ostringstream message;
        message << "root \"" << argument << "\": invalid logical path: "
                << reason;
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }
    std::string access_error;
    if (!resolve_logical(argument, state.search_roots, disk_path,
                         &access_error)) {
        if (!access_error.empty()) {
            return fail(error, argument,
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_GENERIC, access_error);
        }
        std::ostringstream message;
        message << "root \"" << argument << "\" not found; searched "
                << join_quoted(state.search_roots, ", ");
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_MISSING_IMPORT, message.str());
    }
    logical = argument;
    return true;
}

} // namespace

bool load_source_files(const SourceLoaderOptions& options,
                       const std::vector<std::string>& roots,
                       easypb_schema::SchemaSet& result,
                       easypb_schema::Diagnostic& error)
{
    result.clear();
    error = easypb_schema::Diagnostic();

    LoaderState state;
    state.result = &result;
    state.search_roots = options.proto_paths;
    if (state.search_roots.empty()) state.search_roots.push_back(".");
    // An empty entry means the working directory, exactly like ".":
    // join_disk_path already treats "" as cwd, and this keeps
    // lexical_absolute consistent with it.
    state.search_roots_absolute.reserve(state.search_roots.size());
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        if (state.search_roots[i].empty()) state.search_roots[i] = ".";
        state.search_roots_absolute.push_back(
            easypb_file::lexical_absolute(state.search_roots[i]));
    }

    for (std::size_t i = 0; i < roots.size(); ++i) {
        // A failed root clears the partial result so no half-loaded set
        // or generated output can be mistaken for a complete compilation.
        std::string logical;
        std::string disk_path;
        easypb_schema::Diagnostic resolve_error;
        if (!resolve_root(roots[i], state, logical, disk_path,
                          resolve_error)) {
            error = resolve_error;
            result.clear();
            return false;
        }
        if (!load_recursive(logical, disk_path, state, error)) {
            result.clear();
            return false;
        }
        easypb_schema::SchemaFile* target = result.find_file(logical);
        if (target != 0) result.add_target(*target);
    }

    // Complete source loading validates the graph with no missing edges.
    // The name-keyed reservation prevented infinite reads; this common
    // pass owns final cycle validation for every frontend.
    if (!easypb_schema::bind_import_edges(result, false, error)) {
        result.clear();
        return false;
    }
    return true;
}

} // namespace easypb_proto
