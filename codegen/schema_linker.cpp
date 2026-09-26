// Link orchestration for the schema layer: the six stages of the end-state map
// - edge binding, one shared index, visibility, resolution, warning hygiene,
// publication - with the deterministic name-sorted walk (A2) and the
// publish-on-success rule (A1) that hold across all of them. The compatibility
// branch belongs to Task 3 (Step 7); until it lands, the mode refuses loudly
// instead of publishing a half-linked view.

#include "schema_linker.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace easypb_schema {

namespace {

// Each translation unit keeps its own copy, following schema.cpp and
// schema_semantics.cpp: there is no shared declaration for this helper.
std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

// Cycle-free by construction: bind_import_edges rejects cycles before the
// linker runs, so a plain memoized DFS needs no in-progress marker, and the
// entry inserted below is always complete by the time it is read back. Do not
// "fix" this by adding a recursion guard - the guard would mask the only
// interesting bug, which is a memo entry returned half-built.
typedef std::map<const SchemaFile*, std::set<const SchemaFile*> > ExportMemo;

// PublicExports(F) = {F} union PublicExports(P) over public imports P of F.
const std::set<const SchemaFile*>& public_exports(const SchemaFile* file,
                                                  ExportMemo& memo)
{
    ExportMemo::iterator found = memo.find(file);
    if (found != memo.end()) return found->second;
    // Insert first: every caller below concatenates the set it gets back, so
    // the entry must exist before the walk. Because cycles are impossible (see
    // above), nothing can read this entry while it is still being built.
    std::set<const SchemaFile*>& result = memo[file];
    result.insert(file);
    for (std::size_t i = 0; i < file->edges.size(); ++i) {
        const ImportEdge& edge = file->edges[i];
        if (edge.modifier == ImportInfo::PUBLIC_IMPORT && edge.target != 0) {
            const std::set<const SchemaFile*>& sub =
                public_exports(edge.target, memo);
            result.insert(sub.begin(), sub.end());
        }
    }
    return result;
}

// Visible(F) = {F} union PublicExports(D) over every direct import D of F.
// In complete mode a weak import is a required ordinary dependency, so weak
// edges are treated exactly like normal edges here. Edges with target == 0
// exist only in compatibility mode and contribute nothing.
std::set<const SchemaFile*> visible_files(const SchemaFile* file,
                                          ExportMemo& memo)
{
    std::set<const SchemaFile*> result;
    result.insert(file);
    for (std::size_t i = 0; i < file->edges.size(); ++i) {
        const ImportEdge& edge = file->edges[i];
        if (edge.target == 0) continue;
        const std::set<const SchemaFile*>& sub =
            public_exports(edge.target, memo);
        result.insert(sub.begin(), sub.end());
    }
    return result;
}

bool less_by_logical_name(const SchemaFile* left, const SchemaFile* right)
{
    return view_text(left->file.name) < view_text(right->file.name);
}

// The deterministic walk order of stages 1-6: a function of the logical file
// names, never of files.files() insertion order and never of pointer values
// (A2). SchemaSet rejects duplicate logical names, so equal keys cannot
// occur; stable_sort keeps that guarantee explicit - the order stays a
// function of the names alone even if a duplicate ever slipped through.
std::vector<SchemaFile*> files_by_logical_name(const SchemaSet& files)
{
    const std::vector<std::unique_ptr<SchemaFile> >& owned = files.files();
    std::vector<SchemaFile*> ordered;
    ordered.reserve(owned.size());
    for (std::size_t i = 0; i < owned.size(); ++i) {
        ordered.push_back(owned[i].get());
    }
    std::stable_sort(ordered.begin(), ordered.end(), less_by_logical_name);
    return ordered;
}

// A25's refusal: an input-capability statement, not a validation verdict. We
// do not model editions or their features, and protoc decides enum openness
// and field presence from the edition, so a "not proto3 means proto2" test
// would silently downgrade an editions file to a semantics it does not have
// (measured: oracle E8 accepts where E9, the proto2 twin, rejects).
//
// No descriptor path suffix: the file itself is the subject, and the value is
// quoted so the message names what was actually read.
bool fail_unsupported_syntax(const SchemaFile& file, Diagnostic& error)
{
    error = Diagnostic();
    error.file = view_text(file.file.name);
    error.location = unknown_location();
    error.message = "file \"" + view_text(file.file.name) + "\" declares syntax \"" +
                    view_text(file.file.syntax) + "\", which is not supported: " +
                    "only absent, \"proto2\", and \"proto3\" are accepted";
    error.warning = false;
    error.code = DIAGNOSTIC_UNSUPPORTED_SYNTAX;
    return false;
}

// A10's warning hygiene. The standalone parser path resolves with
// allow_unresolved = true (proto_parser.cpp's semantic pass), so a file in the
// set may already carry DIAGNOSTIC_UNRESOLVED_TYPE warnings for references
// this link has just resolved, each paired with a generic warning about the
// default that could not be validated either. A successful complete link
// resolves the type and validates the default, so BOTH are stale and both are
// dropped; every other code is left alone, because a warning of another kind
// is still true. On failure the caller removes nothing, because the warnings
// are part of the explanation (fix round r4, ledger D-13).
void drop_stale_unresolved_warnings(SchemaFile& file)
{
    std::vector<Diagnostic> kept;
    kept.reserve(file.warnings.size());
    for (std::size_t i = 0; i < file.warnings.size(); ++i) {
        const bool stale =
            file.warnings[i].code == DIAGNOSTIC_UNRESOLVED_TYPE ||
            is_deferred_default_warning(file.warnings[i]);
        if (!stale) kept.push_back(file.warnings[i]);
    }
    file.warnings.swap(kept);
}

// Placeholder for Step 7 (Task 3). The compatibility pass is a named policy of
// its own - one descriptor-origin file, missing external definitions permitted
// only when the descriptor classifies them itself, a bounded one-warning-per-
// type rule - and it must not be approximated by running the complete-mode
// pipeline: that would resolve with allow_unresolved = false and misreport the
// thin descriptors this mode exists for as unresolved-type errors. Until Step
// 7 defines it, the mode refuses loudly and, like every other failure path,
// publishes nothing (A1).
bool link_compatibility(SchemaSet& files, LinkedSchema& result,
                        Diagnostic& error)
{
    (void)files;
    (void)result;   // already the invalid empty state, set by the caller
    error = Diagnostic();
    error.message =
        "compatibility mode is not implemented until Step 7 of plan 4";
    error.code = DIAGNOSTIC_GENERIC;
    return false;
}

} // namespace

bool link_schema(SchemaSet& files, LinkMode mode,
                 LinkedSchema& result, Diagnostic& error)
{
    // Publish on success only (A1, E3): start from the invalid empty state, so
    // a failed link can never leave an earlier publication readable.
    result = LinkedSchema();

    // The deterministic walk order of every stage below (A2). It is computed
    // once and taken before stage 1, because the syntax classification has to
    // see the files in the same order the later stages use; binding edges only
    // fills each file's edge list, so the pointers stay valid.
    const std::vector<SchemaFile*> ordered = files_by_logical_name(files);

    // Stage 1 (A25, both modes): classify every file's syntax before stage 2
    // binds edges and before the index is built. Refusing here keeps the
    // capability statement honest - an editions file with no cross-syntax
    // reference would otherwise link silently, because no resolver would ever
    // ask about its syntax.
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (classify_file_syntax(ordered[i]->file) ==
            FILE_SYNTAX_UNSUPPORTED) {
            return fail_unsupported_syntax(*ordered[i], error);
        }
    }

    // Compatibility mode is its own named pass (A6). It performs the same
    // syntax refusal in its preconditions, so the check above stays outside
    // the branch.
    if (mode == LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY) {
        return link_compatibility(files, result, error);
    }

    // Stage 2: bind every file's import edges. In complete mode a missing
    // target is an error.
    if (!bind_import_edges(files, false, error)) return false;

    // Stage 3a (A26): every synthetic map entry present in an available file
    // must carry a field numbered 1 and a field numbered 2, whether or not
    // anything references it. This runs before resolution on purpose: the
    // ownership guard is driven by a referencing field, while a malformed
    // entry is a structural fact about the descriptor and must fail on its
    // own, including in compatibility mode (Step 7.2 repeats the sweep).
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (!validate_map_entry_shapes(*ordered[i], error)) return false;
    }

    // Everything is built into a local and assigned once, at the end (A1).
    LinkedSchema linked;

    // Stage 3: one symbol index over every loaded file. The index is built
    // here, per call, and never cached: a file must be indexed exactly once,
    // and re-indexing one through a surviving index would report its own
    // declarations as duplicates (Step 3.1, Appendix A).
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (!index_file(*ordered[i], linked.symbols, error)) return false;
    }

    // Stage 4: Visible(F) for every file, sharing one public-export memo.
    // Visibility depends on the import graph alone, not on the index.
    ExportMemo memo;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        linked.visible_files[ordered[i]] = visible_files(ordered[i], memo);
    }

    // Stage 5: resolve every file against its own visible set. The linker's
    // answer is authoritative and overwrites anything an earlier local pass
    // may have resolved; allow_unresolved is false, because in complete mode a
    // reference that cannot be resolved is an error. The walk order makes the
    // first reported failure a function of the logical names alone.
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (!resolve_file(*ordered[i], linked.symbols,
                          linked.visible_files[ordered[i]],
                          /*allow_unresolved=*/false, error)) {
            return false;
        }
    }

    // Stage 5b (A10): warning hygiene after the last stage that can add
    // warnings and before publication. Nothing is dropped on a failure path -
    // the loops above return first.
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        drop_stale_unresolved_warnings(*ordered[i]);
    }

    // Stage 6: publish only now. Every stage above built `linked`; a failure
    // returned before this line leaves `result` in the invalid empty state set
    // at entry, never a partial view.
    linked.files = &files;
    linked.complete = true;
    result = linked;   // SymbolIndex/map copies are fine: the files are not.
    return true;
}

} // namespace easypb_schema
