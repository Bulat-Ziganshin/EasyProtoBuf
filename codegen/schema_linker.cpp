// Link orchestration for the schema layer: the six stages of the end-state map
// - edge binding, one shared index, visibility, resolution, warning hygiene,
// publication - with the deterministic name-sorted walk (A2) and the
// publish-on-success rule (A1) that hold across all of them. The compatibility
// branch (Task 3) shares those stages as its own named pass: exactly one
// descriptor-origin file, missing targets tolerated as null-target edges, and
// a finish pass that permits an unresolved reference only when the descriptor
// classifies it itself.

#include "schema_linker.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
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

// 7.1's preconditions: the two ways a set can fail to be the single
// descriptor-origin file this mode's warranty is written for. A mode
// precondition has no dedicated code among plan 4's frozen seven (the verdict
// is neither a lookup outcome nor a syntax statement) and an eighth value
// would be a contract amendment, so these refusals carry DIAGNOSTIC_GENERIC
// with the rule in the message, like the other kind-independent refusals. The
// step notes record the classical review's H-5 question (a distinguishable
// code for the mode's own refusals) as an open item, not a silent choice.
bool fail_compatibility(const std::string& file_name,
                        const std::string& message, Diagnostic& error)
{
    error = Diagnostic();
    error.file = file_name;
    error.location = unknown_location();
    error.message = message;
    error.warning = false;
    error.code = DIAGNOSTIC_GENERIC;
    return false;
}

// Task 3 (Steps 7.1/7.2): the thin single-descriptor workflow. Its
// preconditions are deliberately strict - exactly one descriptor-origin file -
// because the mode tolerates missing dependencies, and without the one-file
// rule it would become the general unresolved-type escape hatch the contract
// forbids. A25's unsupported-syntax refusal is not repeated here:
// link_schema's stage 1 already classified every file of every mode before
// this branch runs, so an editions input is refused with
// DIAGNOSTIC_UNSUPPORTED_SYNTAX in the same place for both modes, before any
// resolution work.
bool link_compatibility(SchemaSet& files, LinkedSchema& result,
                        Diagnostic& error)
{
    // Preconditions (7.1). The walk is name-sorted like every stage, so the
    // first reported failure is a function of the logical names alone (A2).
    const std::vector<SchemaFile*> ordered = files_by_logical_name(files);
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (!ordered[i]->from_descriptor_set) {
            return fail_compatibility(
                view_text(ordered[i]->file.name),
                "compatibility mode is restricted to descriptor-origin input: "
                "file \"" + view_text(ordered[i]->file.name) +
                "\" was produced by a source frontend "
                "(from_descriptor_set == false)",
                error);
        }
    }
    // A26: the precondition is EXACTLY one file. `parser/data/empty.pbs`
    // decodes to zero files and fails the same check as a two-file set, so
    // the message names both violations - "a single file" must not read as
    // "at most one".
    if (ordered.empty()) {
        return fail_compatibility(
            std::string(),
            "compatibility mode links exactly one descriptor-origin file: "
            "this set contains no file at all (a zero-file set fails the same "
            "precondition as a set of two descriptor-origin files)",
            error);
    }
    if (ordered.size() != 1) {
        // easypb::detail::number_to_string, not std::to_string: the supported
        // legacy lane (GCC 4.7.3) has none, and the header-inline helper exists
        // exactly for that (D-18, fix round r9).
        return fail_compatibility(
            std::string(),
            "compatibility mode links exactly one descriptor-origin file: "
            "this set contains " +
                easypb::detail::number_to_string(ordered.size()) +
                " files (a set of two descriptor-origin files fails the same "
                "precondition as a set with no file at all)",
            error);
    }
    SchemaFile& file = *ordered[0];

    // 7.2: missing targets become null-target edges; dependency-index
    // malformation, duplicate symbols and cycles among available files stay
    // errors - allow_missing relaxes missing targets, and nothing else (A18).
    if (!bind_import_edges(files, /*allow_missing=*/true, error)) return false;

    // A26's entry-shape sweep runs before the finish pass, exactly as in
    // complete mode: the malformed shape is a structural fact about an
    // available descriptor, not a by-product of a reference, and
    // `maps/data/malformed_map.pbs` is the fixture consumed here.
    if (!validate_map_entry_shapes(file, error)) return false;

    // Everything is built into a local and assigned once, at the end (A1).
    LinkedSchema linked;
    if (!index_file(file, linked.symbols, error)) return false;

    // Visible(F) degenerates to {F}: with one file in the set every dependency
    // edge is a null-target edge (an edge naming the file itself is a cycle
    // and the bind above refuses it), and null-target edges contribute
    // nothing.
    ExportMemo memo;
    linked.visible_files[&file] = visible_files(&file, memo);

    // The finish pass's caller-owned state (A4/A5): one warning per external
    // type name, and the report that external information was absent.
    std::set<std::string> warned_types;
    bool missing_external = false;
    if (!resolve_file_compat(file, linked.symbols, linked.visible_files[&file],
                             warned_types, missing_external, error)) {
        return false;
    }

    // A4: `complete` describes the input. Any null-target edge produced above
    // or any permitted external reference makes it false; it stays true only
    // for a thin descriptor whose whole dependency graph and every field
    // resolved locally. A10's warning hygiene is not repeated here: a
    // descriptor-origin file carries no stale warnings from the parser's
    // standalone pass, and this mode resolves no external reference whose
    // warning a later pass could make stale.
    bool missing_edge = false;
    for (std::size_t i = 0; i < file.edges.size(); ++i) {
        if (file.edges[i].target == 0) missing_edge = true;
    }

    linked.files = &files;
    linked.complete = !missing_edge && !missing_external;
    result = std::move(linked);
    return true;
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

    // Compatibility mode is its own named pass (A6): preconditions, bind with
    // allow_missing, the entry-shape sweep, index, visibility, the finish pass
    // and publication all live in link_compatibility. The A25 refusal above
    // stays outside the branch because it is an input-capability statement
    // about every file, identical in both modes - which is what refuses an
    // editions `.pbs` here too, before the finish pass can run.
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
    // A move, not a copy (fix round r5, ledger H-8/P-05). `LinkedSchema`
    // declares only a user-provided default constructor, which does not
    // suppress the implicit move assignment, and every member moves in O(1):
    // one pointer, the symbol map, the visibility map, one bool. `linked` is
    // function-local and nothing reads it after this line, so the moved-from
    // state is never observed; the copy this replaces was the whole symbol
    // index plus every visibility set.
    result = std::move(linked);
    return true;
}

} // namespace easypb_schema
