// Link orchestration for the schema layer. Stages 2 (edge binding), 3 (the
// shared symbol index) and 4 (visibility sets) are implemented; stage 5
// (resolution) arrives in a later plan-4 step, so the result this file
// publishes today is deliberately incomplete (see the TEMPORARY note on the
// success path of link_schema).

#include "schema_linker.hpp"

#include <algorithm>
#include <cstddef>

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

// The deterministic walk order of stages 3-5: a function of the logical file
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

} // namespace

bool link_schema(SchemaSet& files, LinkMode mode,
                 LinkedSchema& result, Diagnostic& error)
{
    // Publish on success only (A1, E3): start from the invalid empty state, so
    // a failed link can never leave an earlier publication readable.
    result = LinkedSchema();

    // Stage 2: bind every file's import edges. In compatibility mode a
    // missing target is an expected single-file condition; in complete mode it
    // is an error. Stage 1's mode preconditions are not implemented yet.
    const bool allow_missing = (mode == LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY);
    if (!bind_import_edges(files, allow_missing, error)) return false;

    // Stages 3-5 share this walk order (A2).
    const std::vector<SchemaFile*> ordered = files_by_logical_name(files);

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

    // TEMPORARY (Step 2): stage 5 is not implemented yet; Step 5 replaces this
    // with real resolution and flips complete to true.
    linked.files = &files;
    linked.complete = false;
    result = linked;   // SymbolIndex/map copies are fine: the files are not.
    return true;
}

} // namespace easypb_schema
