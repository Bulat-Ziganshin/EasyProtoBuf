#include "proto_loader.hpp"

#include "file_paths.hpp"
#include "logical_paths.hpp"
#include "proto_parser.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace easypb_proto {

// W-35: the guarded conversion lives in schema.hpp, beside the storage it
// converts; one door for every frontend.
using easypb_schema::view_text;

namespace {

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
    // True when the root list was synthesized because the caller named none.
    // The implicit working directory is the reference compiler's default
    // `--proto_path`, but its textual mapping never contains an absolute
    // input, so an absolute argument still needs an explicit root here
    // (review 1027, U-59).
    bool implicit_root;
    // Discovery path from the selected root to the file being loaded,
    // used only to spell diagnostics.
    std::vector<std::string> chain;

    LoaderState() : result(0), implicit_root(false) {}
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
            // The renderer already prints the file label; the reason names
            // no path (W-19/W-29). The chain suffix stays.
            std::ostringstream message;
            message << read_error;
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
        // W-61: the outer Diagnostic already carries the file and the position,
        // and the renderer prints `file:line:column: ` in front of the message,
        // so this wrapper names only the logical file, the import chain and the
        // parser's own text. Re-embedding parse_error.file/location here made
        // the reader see the position twice.
        std::ostringstream message;
        message << "while loading \"" << logical << "\"";
        if (!chain.empty()) {
            message << " (chain " << chain_text(chain, logical) << ")";
        }
        message << ": " << parse_error.message;
        return fail(error, parse_error.file.empty()
                                       ? logical
                                       : parse_error.file,
                    parse_error.location, parse_error.code,
                    message.str());
    }
    return true;
}

// One file on the loader's explicit worklist: the slot reserved for it and the
// position of the next dependency to follow. The stack of frames is the
// discovery path, and `LoaderState::chain` mirrors it by logical name.
struct LoadFrame
{
    easypb_schema::SchemaFile* file;
    std::size_t next_dependency;

    LoadFrame(easypb_schema::SchemaFile* slot, std::size_t next)
        : file(slot), next_dependency(next) {}
};

// The chain-depth refusal (review 1027, U-05/U-06). The sentence names the
// offending file, the depth it would reach, the limit, the import that asks
// for it and the root of this discovery path. The whole chain is deliberately
// NOT printed: it is the input whose length is the problem, and a suffix of
// 1 001 names would bury the four facts a reader needs.
bool fail_chain_depth(const std::string& logical,
                      std::size_t depth,
                      const std::string& importer,
                      const LoaderState& state,
                      easypb_schema::Diagnostic& error)
{
    std::ostringstream message;
    message << "import chain depth " << depth << " exceeds the limit of "
            << easypb_schema::max_import_chain_depth() << " files";
    if (!state.chain.empty()) {
        message << " (imported by \"" << importer << "\"; the chain starts at \""
                << state.chain.front() << "\")";
    }
    return fail(error, logical, easypb_schema::unknown_location(),
                easypb_schema::DIAGNOSTIC_GENERIC, message.str());
}

// Pushes one file onto the worklist: the depth bound first, because a file
// beyond it is never read; then the reserved slot, the deferred parse and the
// frame. The slot is reserved before its imports are followed so repeated and
// cyclic references cannot cause repeated parsing or infinite reads.
bool begin_frame(const std::string& logical,
                 const std::string& disk_path,
                 LoaderState& state,
                 std::vector<LoadFrame>& stack,
                 easypb_schema::Diagnostic& error)
{
    const std::size_t depth = state.chain.size() + 1;
    if (depth > easypb_schema::max_import_chain_depth()) {
        return fail_chain_depth(logical, depth,
                                state.chain.empty() ? std::string()
                                                    : state.chain.back(),
                                state, error);
    }

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
    stack.push_back(LoadFrame(slot, 0));
    return true;
}

// Follows one import of one file: validates the spelling, resolves it through
// the search roots, and pushes it onto the worklist when it is not loaded yet.
// Everything else about the import - positions, the weak/public modifier, the
// diagnostics - is the same for every frame, so it stays in one place.
bool follow_import(const easypb_schema::SchemaFile& importer_file,
                   std::size_t index,
                   LoaderState& state,
                   std::vector<LoadFrame>& stack,
                   easypb_schema::Diagnostic& error)
{
    const std::string importer = view_text(importer_file.file.name);
    const std::string dependency =
        view_text(importer_file.file.dependency[index]);
    const easypb_schema::SourceLocation where =
        import_location(importer_file, index, dependency);
    // The descriptor lists are authoritative for graph meaning;
    // imports supplies positions only, so the weak/public modifiers
    // come from weak_dependency/public_dependency rather than the
    // positional imports entry. This matches the binder wording
    // ("missing public import") for all three modifiers.
    bool is_weak = false;
    bool is_public = false;
    for (std::size_t k = 0; k < importer_file.file.weak_dependency.size();
         ++k) {
        if (importer_file.file.weak_dependency[k] ==
            static_cast<std::int32_t>(index)) {
            is_weak = true;
            break;
        }
    }
    for (std::size_t k = 0; k < importer_file.file.public_dependency.size();
         ++k) {
        if (importer_file.file.public_dependency[k] ==
            static_cast<std::int32_t>(index)) {
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

    // The dependency is loaded already (a diamond, a repeated root or a file
    // another target already reached): it stays loaded once and is not walked
    // again.
    if (state.result->find_file(dependency) != 0) return true;

    return begin_frame(dependency, child_disk, state, stack, error);
}

bool load_chain(const std::string& logical,
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

    // The traversal is a heap worklist, not one C++ call frame per import
    // edge (review 1027, U-05/U-06): an untrusted chain must not be able to
    // overflow the process stack, and the chain-depth bound below is a policy
    // limit rather than the stack's own. Frames are expanded in dependency
    // declaration order, depth first, exactly like the recursive walk this
    // replaces, so every diagnostic and the order files are read in are
    // unchanged.
    const std::size_t chain_base = state.chain.size();
    std::vector<LoadFrame> stack;
    if (!begin_frame(logical, disk_path, state, stack, error)) {
        state.chain.resize(chain_base);
        return false;
    }
    while (!stack.empty()) {
        LoadFrame& frame = stack.back();
        if (frame.next_dependency >= frame.file->file.dependency.size()) {
            state.chain.pop_back();
            stack.pop_back();
            continue;
        }
        const std::size_t index = frame.next_dependency++;
        if (!follow_import(*frame.file, index, state, stack, error)) {
            state.chain.resize(chain_base);
            return false;
        }
    }
    return true;
}

// Turn one command-line root into a logical name plus its disk path. An
// argument that names an existing file is used through search-root mapping;
// when no root contains it, the spelling is retried as a logical name, which
// is what the reference compiler does with an argument it cannot map. An
// argument that names no file is a logical name resolved through the ordered
// roots. A denied access is a failure in both shapes, never a fall-through to
// the other one.

// The failure shared by the relative argument shapes that lie under no
// search root. The reference compiler reports the same state as "File does
// not reside within any path specified using --proto_path (or -I)"; an
// absolute argument uses that sentence verbatim below, so the reader can
// compare it with protoc's own transcript (review 1027, U-59).
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

// An absolute argument outside every explicit search root. The reference
// compiler prints this sentence from its NO_MAPPING branch; its default "."
// mapping is textual and never contains an absolute spelling, which is why
// an absolute input must name an explicit -I root. The loader matches
// normalized roots instead of raw spellings, but requires the same explicit
// root (review 1027, U-59).
bool fail_absolute_outside_search_roots(const std::string& argument,
                                        easypb_schema::Diagnostic& error)
{
    return fail(error, argument, easypb_schema::unknown_location(),
                easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                "File does not reside within any path specified using "
                "--proto_path (or -I). You must specify a --proto_path which "
                "encompasses this file");
}

// True when an existing path lies under one of the search roots, using
// the same byte-wise root mapping as the rest of the loader. A path equal
// to a root lies inside it: the empty remainder names the root directory
// itself, which is a directory rather than an unmapped file (review 1027,
// U-28). `strip_search_root_prefix` keeps strict containment because an
// empty remainder is not a valid logical name.
bool lies_under_search_root(const std::string& path,
                            const LoaderState& state,
                            std::string* containing_root)
{
    const std::string absolute = easypb_file::lexical_absolute(path);
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        std::string remainder;
        if (absolute != state.search_roots_absolute[i] &&
            !easypb_file::strip_search_root_prefix(
                absolute,
                state.search_roots_absolute[i],
                remainder)) {
            continue;
        }
        if (containing_root != 0) {
            *containing_root = state.search_roots[i];
        }
        return true;
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
    // A relative spelling cannot be answered without a working directory.
    // Refuse it before the status and containment branches: neither may turn
    // the unknown cwd into an answer (a logical name under a root, or "lies
    // outside all search roots"). An absolute spelling is unaffected.
    if (easypb_file::lexical_absolute(argument).empty()) {
        std::ostringstream message;
        message << "root \"" << argument
                << "\": cannot resolve a relative spelling "
                   "(the current working directory is unknown)";
        return fail(error, argument, easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }
    const bool absolute_argument = easypb_file::is_absolute_disk_path(argument);
    std::string status_error;
    const easypb_file::FileStatus root_status =
        easypb_file::disk_file_status(argument, status_error);
    if (root_status == easypb_file::FILE_STATUS_ERROR) {
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_GENERIC, status_error);
    }
    if (root_status == easypb_file::FILE_STATUS_FILE) {
        // An absolute input must name an explicit search root. The implicit
        // working directory is the default only for logical names: protoc's
        // textual mapping never contains an absolute spelling, so
        // `codegen /abs/a.proto` with no -I is refused even when the file is
        // physically under the working directory (review 1027, U-59).
        if (absolute_argument && state.implicit_root) {
            return fail_absolute_outside_search_roots(argument, error);
        }
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
        case MAP_OUTSIDE: {
            // An absolute spelling cannot be read as a virtual name, so it
            // is not retried as one; it keeps the reference sentence.
            if (absolute_argument) {
                return fail_absolute_outside_search_roots(argument, error);
            }
            // The reference compiler's NO_MAPPING answer: a spelling that
            // names an existing file under no search root is retried as a
            // virtual (logical) name through the ordered roots before the run
            // gives up (command_line_interface.cc:1721-1741). A stale copy in
            // the working directory must not turn the file the roots name into
            // a refusal, and the logical name stays the spelling, so the
            // generated output name does too. A spelling that also fails the
            // logical-name rules (an absolute path, a parent reference, a
            // backslash) has no virtual reading and keeps the outside-roots
            // diagnostic.
            std::string reason;
            if (easypb_schema::validate_logical_path(argument, reason)) {
                std::string logical_disk;
                std::string access_error;
                if (resolve_logical(argument, state.search_roots, logical_disk,
                                    &access_error)) {
                    logical = argument;
                    disk_path = logical_disk;
                    return true;
                }
                if (!access_error.empty()) {
                    return fail(error, argument,
                                easypb_schema::unknown_location(),
                                easypb_schema::DIAGNOSTIC_GENERIC,
                                access_error);
                }
            }
            return fail_outside_search_roots(argument, state, error);
        }
        }
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
        if (absolute_argument) {
            return fail_absolute_outside_search_roots(argument, error);
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
    state.implicit_root = options.proto_paths.empty();
    if (state.search_roots.empty()) state.search_roots.push_back(".");
    // An empty entry means the working directory, exactly like ".":
    // join_disk_path already treats "" as cwd, and this keeps
    // lexical_absolute consistent with it.
    state.search_roots_absolute.reserve(state.search_roots.size());
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        if (state.search_roots[i].empty()) state.search_roots[i] = ".";
        // A drive-relative root ("C:foo", "C:") is anchored to the per-drive
        // current directory, so one spelling can name a different tree from
        // run to run; the reference compiler refuses the form, and so does
        // this loader (review 1027, U-60). A drive-absolute ("C:/foo") or a
        // plain relative spelling is unaffected.
        if (easypb_file::is_drive_relative_disk_path(state.search_roots[i])) {
            return fail(error, state.search_roots[i],
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        "drive-relative spelling is not a search root; use "
                        "an absolute path or a path relative to the working "
                        "directory");
        }
        const std::string absolute =
            easypb_file::lexical_absolute(state.search_roots[i]);
        if (absolute.empty()) {
            // A root that cannot be made absolute must not become a root that
            // matches nothing (or, worse, the filesystem root): the process
            // has no working directory to anchor a relative root at.
            std::ostringstream message;
            message << "search root \"" << state.search_roots[i]
                    << "\": cannot resolve a relative spelling "
                       "(the current working directory is unknown)";
            return fail(error, state.search_roots[i],
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        message.str());
        }
        state.search_roots_absolute.push_back(absolute);
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
        if (!load_chain(logical, disk_path, state, error)) {
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
