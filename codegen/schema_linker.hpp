#ifndef EASYPB_SCHEMA_LINKER_HPP_INCLUDED
#define EASYPB_SCHEMA_LINKER_HPP_INCLUDED

#include <map>
#include <set>

#include "schema.hpp"
#include "schema_semantics.hpp"

namespace easypb_schema {

enum LinkMode {
    LINK_COMPLETE,
    LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY
};

// Non-owning view of a caller-owned, frozen SchemaSet. Name keys/sets are
// owned; referenced files/descriptors are not copied. The owner must outlive
// the result and every generator call. Published only after successful
// linking; a default-constructed result is the invalid empty state.
struct LinkedSchema {
    const SchemaSet* files;      // null in the invalid empty state
    SymbolIndex symbols;
    std::map<const SchemaFile*, std::set<const SchemaFile*> > visible_files;
    bool complete;

    // The plan-4 contract declares this struct without a constructor; the
    // user-provided default constructor is a C++11 requirement, not drift:
    // a const-qualified or statically allocated LinkedSchema cannot be
    // default-initialized without it, which is why SymbolIndex carries one
    // too. The consequence is that LinkedSchema is no longer an aggregate, so
    // it cannot be brace-initialized; files == 0 remains the documented
    // invalid-empty marker the publish-on-success rule depends on.
    LinkedSchema() : files(0), complete(false) {}
};

// Links all files in the set according to mode. On success fills result and
// returns true; on failure fills error, leaves result in the invalid empty
// state, and returns false. Never publish a partially linked result.
//
// Ownership is mutation - the link writes into the CALLER'S set. Every
// field it resolves gets `type_name` rewritten to the index's absolute
// spelling and `type`/`has_type` set to the resolved kind (a proto2 group
// keeps its declared TYPE_GROUP, because that is what its wire encoding
// needs), so a set that was linked once no longer matches the bytes it was
// loaded from. One set, one link (H-6).
//
// A FAILED link mutates too (ledger `D-60`): the stages that ran before the
// failure have already rewritten the fields they walked and bound the import
// edges, so "link failed" must not be read as "the input is unchanged". A
// retry is convergent - the same set linked again reaches the same verdict,
// and a link that succeeds is idempotent - so a caller that needs the original
// bytes must reload them rather than rely on the failure having left them.
bool link_schema(SchemaSet& files, LinkMode mode,
                 LinkedSchema& result, Diagnostic& error);

} // namespace easypb_schema

#endif
