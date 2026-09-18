#include "proto_loader.hpp"

#include "file_paths.hpp"
#include "logical_paths.hpp"
#include "proto_parser.hpp"

#include <cstdint>
#include <map>
#include <sstream>
#include <utility>
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

struct IdentityCompare {
    bool operator()(const easypb_file::FileIdentity& left,
                    const easypb_file::FileIdentity& right) const
    {
        return easypb_file::identity_less(left, right);
    }
};

// Native identity to the first logical name seen for it. Only
// successfully queried identities enter these maps.
typedef std::map<easypb_file::FileIdentity,
                 std::string,
                 IdentityCompare> IdentityNameMap;

struct LoaderState {
    std::vector<std::string> search_roots;
    easypb_schema::SchemaSet* result;
    LoadStatistics* statistics;
    IdentityNameMap identity_to_logical;
    std::map<std::string, easypb_file::FileIdentity> logical_to_identity;
    std::vector<std::string> chain;
    // Native identity of every isolated root to its first requested
    // spelling. Only lexically equivalent spellings (".", separators,
    // case on Windows) deduplicate to it; different names of one file
    // are reported as a duplicate alias instead, and different files
    // never share an entry.
    IdentityNameMap isolated_by_identity;
    // Discovery path from the selected root to every loaded file,
    // recorded before following imports. Binder failures name only the
    // failing file, so this reattaches the root-to-failure chain.
    std::map<std::string, std::vector<std::string> > discovery_chain;

    LoaderState() : result(0), statistics(0) {}
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
// with the winning disk path and identity; the first matching root wins
// and is never substituted based on the importing file's location.
// When the first matching file exists but its identity cannot be
// queried, the error is stored in identity_error and false is returned
// immediately: falling through to a later root would violate
// first-match-wins precedence.
bool resolve_logical(const std::string& logical,
                     const std::vector<std::string>& search_roots,
                     std::string& disk_path,
                     easypb_file::FileIdentity& identity,
                     std::string* identity_error)
{
    if (identity_error != 0) identity_error->clear();
    for (std::size_t i = 0; i < search_roots.size(); ++i) {
        const std::string candidate =
            easypb_file::join_disk_path(search_roots[i], logical);
        // An operational probe failure stops the search instead of
        // moving to the next root: only absence advances, so a
        // first-root failure can never silently select another file.
        std::string status_error;
        const easypb_file::FileStatus status =
            easypb_file::disk_file_status(candidate, status_error);
        if (status == easypb_file::FILE_STATUS_ERROR) {
            if (identity_error != 0) *identity_error = status_error;
            return false;
        }
        if (status != easypb_file::FILE_STATUS_FILE) continue;
        std::string query_error;
        if (!easypb_file::get_file_identity(candidate, identity,
                                            query_error)) {
            if (identity_error != 0) *identity_error = query_error;
            return false;
        }
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

// Map an existing disk path to a logical name under a search root and
// verify it against search-root precedence. Returns true and stores the
// logical name, the precedence-resolved disk path and its already
// queried identity on success (no refresh needed: the identity comes
// from the resolved path itself).
// Returns false without touching error when the path lies under no
// search root; returns false with reported_error set and error filled
// when mapping fails due to shadowing or file identity I/O.
bool map_disk_to_logical(const std::string& argument,
                         const LoaderState& state,
                         const easypb_file::FileIdentity& argument_identity,
                         std::string& logical,
                         std::string& resolved_disk,
                         easypb_file::FileIdentity& resolved_identity,
                         bool& reported_error,
                         easypb_schema::Diagnostic& error)
{
    const std::string file_absolute = easypb_file::lexical_absolute(argument);
    reported_error = false;
    // A file physically under a search root whose remainder is not a
    // valid logical name must not fall through to the isolated fallback:
    // the first such root is remembered while narrower later roots may
    // still yield a valid mapping.
    bool saw_invalid_candidate = false;
    std::string invalid_candidate;
    std::string invalid_root;
    std::string invalid_reason;
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        const std::string root_absolute =
            easypb_file::lexical_absolute(state.search_roots[i]);
        // Case-insensitive on Windows (drive-letter or directory case
        // differences from tab-completion still map to the same logical
        // name); exact on POSIX. Roots that already end in '/' match
        // without a doubled separator. A failed case-sensitivity query
        // is an operational error, not an "outside" answer: it must
        // surface as a diagnostic and never fall through to the
        // isolated fallback below.
        std::string candidate;
        bool prefix_error = false;
        bool fold_used = false;
        if (!easypb_file::strip_search_root_prefix(file_absolute,
                                                   root_absolute,
                                                   candidate,
                                                   &prefix_error,
                                                   &fold_used)) {
            if (prefix_error) {
                std::ostringstream message;
                message << "root \"" << argument
                        << "\": cannot query case sensitivity under search "
                           "root \"" << state.search_roots[i] << "\"";
                error.file = argument;
                error.location = easypb_schema::unknown_location();
                error.message = message.str();
                error.warning = false;
                error.code = easypb_schema::DIAGNOSTIC_GENERIC;
                reported_error = true;
                return false;
            }
            continue;
        }
        if (fold_used) {
            // A fold-based prefix match is only a candidate: the
            // locale-invariant table folds characters (e.g. Deseret)
            // that filesystems do not. Confirm entry-by-entry that the
            // root's spelling walks the same entries; a mismatch means
            // the file is not under this root at all (never an invalid
            // remainder, never a shadow): keep looking, including the
            // isolated fallback below. Only confirmed absence continues
            // quietly; any other confirmation failure is operational
            // and must never degrade into an isolated success.
            bool confirm_error = false;
            if (!easypb_file::confirm_fold_containment(
                    root_absolute, file_absolute, &confirm_error)) {
                if (confirm_error) {
                    std::ostringstream message;
                    message << "root \"" << argument
                            << "\": cannot confirm containment under search "
                               "root \"" << state.search_roots[i] << "\"";
                    error.file = argument;
                    error.location = easypb_schema::unknown_location();
                    error.message = message.str();
                    error.warning = false;
                    error.code = easypb_schema::DIAGNOSTIC_GENERIC;
                    reported_error = true;
                    return false;
                }
                continue;
            }
        }
        // Disk-derived candidates share the single UTF-8 logical
        // namespace: CLI inputs enter transcoded, sources are
        // conventionally UTF-8, so the portable byte rule is exact
        // (UTF-8 trail bytes never collide with separators).
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
        easypb_file::FileIdentity winner_identity;
        std::string identity_error;
        if (!resolve_logical(candidate, state.search_roots, winner_disk,
                             winner_identity, &identity_error)) {
            if (!identity_error.empty()) {
                error.file = argument;
                error.location = easypb_schema::unknown_location();
                error.message = identity_error;
                error.warning = false;
                error.code = easypb_schema::DIAGNOSTIC_GENERIC;
                reported_error = true;
                return false;
            }
            continue;
        }
        if (!easypb_file::same_identity(argument_identity, winner_identity)) {
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
            reported_error = true;
            return false;
        }
        logical = candidate;
        resolved_disk = winner_disk;
        resolved_identity = winner_identity;
        return true;
    }
    if (saw_invalid_candidate) {
        std::ostringstream message;
        message << "root \"" << argument << "\" lies under search root \""
                << invalid_root << "\" but maps to invalid logical name \""
                << invalid_candidate << "\": " << invalid_reason;
        reported_error = true;
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }
    return false;
}

bool load_recursive(const std::string& logical,
                    const std::string& disk_path,
                    const easypb_file::FileIdentity& identity,
                    LoaderState& state,
                    easypb_schema::Diagnostic& error,
                    bool is_isolated_root,
                    const easypb_schema::SourceLocation& import_site);

// Reads one disk file and fills its reserved slot with deferred parsing.
// The temporary source buffer is local to this function, so its lifetime
// verifiably ends on return, before the caller follows imports: in C++11
// shrink_to_fit is only a non-binding request and cannot guarantee that
// ancestor buffers are released during a deep import chain.
bool read_and_parse_source(const std::string& logical,
                           const std::string& disk_path,
                           const std::vector<std::string>& chain,
                           easypb_schema::SchemaFile& slot,
                           LoadStatistics* statistics,
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
    if (statistics != 0) ++statistics->files_read;

    easypb_schema::Diagnostic parse_error;
    easypb_proto::ParseOptions options;
    options.defer_type_resolution = true;
    // Deferred parsing copies every retained string into the file's
    // pool, so nothing below borrows from the temporary buffer.
    const bool ok = easypb_proto::parse_proto(
        logical, contents.data(), contents.size(), slot, parse_error,
        options);
    // The parser resets physical_name to the logical name; retain
    // the physical disk path for diagnostics and alias checks.
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
    if (statistics != 0) ++statistics->files_parsed;
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
            std::vector<std::string> full = state.chain;
            full.push_back(dependency);
            std::ostringstream message;
            message << "file \"" << importer << "\": invalid import \""
                    << dependency << "\": " << reason;
            if (!state.chain.empty()) {
                message << " (chain " << chain_text(state.chain, dependency) << ")";
            }
            return fail(error, importer, where,
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        message.str());
        }

        std::string child_disk;
        easypb_file::FileIdentity child_identity;
        std::string child_identity_error;
        if (!resolve_logical(dependency, state.search_roots, child_disk,
                             child_identity, &child_identity_error)) {
            if (!child_identity_error.empty()) {
                std::ostringstream message;
                message << "file \"" << importer << "\": cannot resolve "
                        << modifier << "import \"" << dependency
                        << "\": " << child_identity_error
                        << "; searched "
                        << join_quoted(state.search_roots, ", ")
                        << "; chain "
                        << chain_text(state.chain, dependency);
                return fail(error, importer, where,
                            easypb_schema::DIAGNOSTIC_GENERIC,
                            message.str());
            }
            std::vector<std::string> full = state.chain;
            full.push_back(dependency);
            std::ostringstream message;
            message << "file \"" << importer << "\": missing "
                    << modifier << "import \"" << dependency
                    << "\"; searched " << join_quoted(state.search_roots, ", ");
            message << "; chain " << chain_text(state.chain, dependency);
            // Isolated absolute parents cannot resolve siblings without
            // an explicit search root: explain the necessary -I.
            if (easypb_file::is_absolute_disk_path(importer)) {
                message << "; add the directory containing \"" << dependency
                        << "\" with -I";
            } else {
                message << "; use -I to add the directory containing \""
                        << dependency << "\"";
            }
            return fail(error, importer, where,
                        easypb_schema::DIAGNOSTIC_MISSING_IMPORT,
                        message.str());
        }

        if (!load_recursive(dependency, child_disk, child_identity, state,
                            error, false, where)) {
            return false;
        }
    }
    return true;
}

bool load_recursive(const std::string& logical,
                    const std::string& disk_path,
                    const easypb_file::FileIdentity& identity,
                    LoaderState& state,
                    easypb_schema::Diagnostic& error,
                    bool is_isolated_root,
                    const easypb_schema::SourceLocation& import_site)
{
    // Same logical name already present: the first file wins. An
    // identical root is deduplicated; a different physical file under
    // the same logical name is a shadowed compilation, not a rename.
    easypb_schema::SchemaFile* existing = state.result->find_file(logical);
    if (existing != 0) {
        const std::map<std::string, easypb_file::FileIdentity>::const_iterator
            known = state.logical_to_identity.find(logical);
        if (known != state.logical_to_identity.end() &&
            easypb_file::same_identity(known->second, identity)) {
            return true;
        }
        std::ostringstream message;
        message << "logical name \"" << logical << "\" from \"" << disk_path
                << "\" is shadowed by the already loaded file \""
                << existing->physical_name
                << "\" (search-root precedence wins)";
        return fail(error, logical,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }

    // One physical source under two logical names is rejected with both
    // names explained, whether the alias comes from symlinks, hardlinks,
    // "./x" spellings, or Windows case variants. A nested import also
    // names its importer, the declaration site, and the path from the
    // selected root; a repeated CLI root has no import site and keeps
    // the plain form.
    {
        const std::map<easypb_file::FileIdentity, std::string,
                       IdentityCompare>::const_iterator found =
            state.identity_to_logical.find(identity);
        if (found != state.identity_to_logical.end() &&
            found->second != logical) {
            std::ostringstream message;
            if (!state.chain.empty()) {
                message << "file \"" << state.chain.back() << "\": import \""
                        << logical << "\": ";
            }
            message << "one physical file appears under two logical names: \""
                    << found->second << "\" and \"" << logical << "\" (\""
                    << disk_path << "\")";
            if (!state.chain.empty()) {
                message << "; chain " << chain_text(state.chain, logical);
            }
            const bool nested = !state.chain.empty();
            return fail(error,
                        nested ? state.chain.back() : logical,
                        nested ? import_site
                               : easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                        message.str());
        }
    }

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
    state.identity_to_logical[identity] = logical;
    state.logical_to_identity[logical] = identity;
    slot->physical_name = disk_path;
    // Snapshot the discovery path from the selected root to this file.
    // Binder failures name only the failing file, so the snapshot
    // reattaches the root-to-failure chain after edges are cleared.
    {
        std::vector<std::string> discovery = state.chain;
        discovery.push_back(logical);
        state.discovery_chain[logical] = discovery;
    }

    // Read and deferred-parse inside a helper whose local source buffer
    // is destroyed on return, before dependencies are followed below.
    // Later reads, metadata, and name lookups must remain valid after
    // pool and vector growth.
    if (!read_and_parse_source(logical, disk_path, state.chain, *slot,
                               state.statistics, error)) {
        return false;
    }

    // The absolute-path convenience covers isolated CLI roots without
    // imports only. Fail here, right after parsing, so dependencies of
    // a doomed root are neither read nor counted.
    if (is_isolated_root && !slot->file.dependency.empty()) {
        const std::string first =
            view_text(slot->file.dependency[0]);
        std::ostringstream message;
        message << "root \"" << logical
                << "\" lies outside all search roots ("
                << join_quoted(state.search_roots, ", ")
                << ") and imports \""
                << first
                << "\"; add the file's directory with -I and pass its "
                   "logical name instead";
        return fail(error, logical, import_location(*slot, 0, first),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }

    state.chain.push_back(logical);
    const bool dependencies_ok = load_dependencies(*slot, state, error);
    state.chain.pop_back();
    return dependencies_ok;
}

bool resolve_root(const std::string& argument, LoaderState& state,
                  std::string& logical, std::string& disk_path,
                  easypb_file::FileIdentity& identity, bool& is_isolated,
                  easypb_schema::Diagnostic& error)
{
    is_isolated = false;
    // An existing disk path is tried first; otherwise the argument is a
    // logical name resolved through the ordered search roots. Disk-first
    // is deliberate per the plan: a spelling that names a real file is a
    // disk-path root even when it would also be a valid logical name.
    // An operational probe failure is neither: it fails loudly instead
    // of sliding into logical-name resolution.
    std::string status_error;
    const easypb_file::FileStatus root_status =
        easypb_file::disk_file_status(argument, status_error);
    if (root_status == easypb_file::FILE_STATUS_ERROR) {
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_GENERIC, status_error);
    }
    if (root_status == easypb_file::FILE_STATUS_FILE) {
        std::string identity_error;
        if (!easypb_file::get_file_identity(argument, identity,
                                            identity_error)) {
            return fail(error, argument,
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_GENERIC, identity_error);
        }
        std::string mapped;
        std::string resolved;
        easypb_file::FileIdentity resolved_identity;
        bool reported_error = false;
        // map_disk_to_logical reports shadowing and identity failures
        // through reported_error; a plain false means no mapping.
        if (map_disk_to_logical(argument, state, identity, mapped, resolved,
                                resolved_identity, reported_error, error)) {
            logical = mapped;
            disk_path = resolved;
            identity = resolved_identity;
            return true;
        }
        if (reported_error) return false;
        if (easypb_file::is_fully_qualified_disk_path(argument)) {
            // Isolated absolute convenience for a root without imports
            // outside all search roots. Only fully qualified paths
            // qualify: drive-relative ("C:x") and rooted ("\x") forms
            // resolve against per-drive/current directories, so their
            // spellings are not stable logical names. The original
            // spelling is kept for standalone stdout generation; a later
            // plan requires a portable logical name for --out-dir in all
            // cases.
            // Only lexically equivalent spellings of one isolated file
            // ("abs/f.proto" versus "abs/./f.proto", case or separators
            // on Windows) share the first spelling as the logical name.
            // Genuinely different names of one physical file (hardlinks,
            // symlinks) are a duplicate alias and name both spellings.
            // 'identity' has already been queried successfully for
            // 'argument' at the beginning of resolve_root().
            const IdentityNameMap::const_iterator entry =
                state.isolated_by_identity.find(identity);
            if (entry != state.isolated_by_identity.end()) {
                // Folding-based equivalence additionally requires the
                // same entries: distinct entries of one file (e.g. a
                // Deseret hardlink pair) are a duplicate alias, not a
                // silent dedup into one target.
                bool entry_error = false;
                if (easypb_file::equivalent_disk_spelling(entry->second,
                                                          argument) &&
                    easypb_file::fold_spellings_same_entry(entry->second,
                                                           argument,
                                                           &entry_error)) {
                    // Preserve the first requested name for standalone
                    // output.
                    logical = entry->second;
                } else if (entry_error) {
                    std::ostringstream message;
                    message << "root \"" << argument
                            << "\": cannot confirm isolated spelling \""
                            << entry->second << "\" names the same entry";
                    return fail(error, argument,
                                easypb_schema::unknown_location(),
                                easypb_schema::DIAGNOSTIC_GENERIC,
                                message.str());
                } else {
                    std::ostringstream message;
                    message << "one physical file appears under two logical "
                            << "names: \"" << entry->second << "\" and \""
                            << argument << "\"";
                    return fail(error, argument,
                                easypb_schema::unknown_location(),
                                easypb_schema::DIAGNOSTIC_INVALID_IMPORT,
                                message.str());
                }
            } else {
                state.isolated_by_identity[identity] = argument;
                logical = argument;
            }

            // Keep the current argument and its already verified identity
            // together. Do not replace identity with that of a cached path.
            disk_path = argument;

            is_isolated = true;
            return true;
        }
        std::ostringstream message;
        message << "root \"" << argument
                << "\" lies outside all search roots (" 
                << join_quoted(state.search_roots, ", ")
                << "); add its directory with -I or pass its logical name";
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }

    std::string reason;
    if (!easypb_schema::validate_logical_path(argument, reason)) {
        // A non-existent disk path that is also not a valid logical name
        // is reported as an invalid root spelling.
        std::ostringstream message;
        message << "root \"" << argument << "\": invalid logical path: "
                << reason;
        return fail(error, argument,
                    easypb_schema::unknown_location(),
                    easypb_schema::DIAGNOSTIC_INVALID_IMPORT, message.str());
    }
    std::string identity_error;
    if (!resolve_logical(argument, state.search_roots, disk_path, identity,
                         &identity_error)) {
        if (!identity_error.empty()) {
            return fail(error, argument,
                        easypb_schema::unknown_location(),
                        easypb_schema::DIAGNOSTIC_GENERIC, identity_error);
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
                       easypb_schema::Diagnostic& error,
                       LoadStatistics* statistics)
{
    result.clear();
    error = easypb_schema::Diagnostic();
    if (statistics != 0) *statistics = LoadStatistics();

    LoaderState state;
    state.result = &result;
    state.statistics = statistics;
    state.search_roots = options.proto_paths;
    if (state.search_roots.empty()) state.search_roots.push_back(".");
    // An empty entry means the working directory, exactly like ".":
    // join_disk_path already treats "" as cwd, and this keeps
    // lexical_absolute consistent with it.
    for (std::size_t i = 0; i < state.search_roots.size(); ++i) {
        if (state.search_roots[i].empty()) state.search_roots[i] = ".";
    }

    for (std::size_t i = 0; i < roots.size(); ++i) {
        // A failed root clears the partial result so no half-loaded set
        // or generated output can be mistaken for a complete compilation.
        std::string logical;
        std::string disk_path;
        easypb_file::FileIdentity identity;
        bool is_isolated = false;
        easypb_schema::Diagnostic resolve_error;
        if (!resolve_root(roots[i], state, logical, disk_path,
                          identity, is_isolated, resolve_error)) {
            error = resolve_error;
            result.clear();
            return false;
        }
        if (!load_recursive(logical, disk_path, identity, state, error,
                            is_isolated,
                            easypb_schema::unknown_location())) {
            result.clear();
            return false;
        }
        easypb_schema::SchemaFile* target = result.find_file(logical);
        if (target != 0) result.add_target(*target);
    }

    // Complete source loading validates the graph with no missing edges.
    // The discovery cache prevented infinite reads; this common pass
    // owns final cycle validation for every frontend.
    if (!easypb_schema::bind_import_edges(result, false, error)) {
        // Binder errors name only the failing file; reattach the
        // discovery path from the selected root so structural failures
        // keep the root-to-failure chain required by the plan. Note the
        // style differs on purpose: the binder spells its cycle path
        // unquoted ("a -> b -> a") while this chain uses the quoted
        // join_quoted style of the discovery diagnostics.
        if (!error.file.empty()) {
            const std::map<std::string,
                           std::vector<std::string> >::const_iterator found =
                state.discovery_chain.find(error.file);
            if (found != state.discovery_chain.end() &&
                found->second.size() > 1) {
                error.message += "; import chain " +
                                 join_quoted(found->second, " -> ");
            }
        }
        result.clear();
        return false;
    }
    return true;
}

} // namespace easypb_proto
