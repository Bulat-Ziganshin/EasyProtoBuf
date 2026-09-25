// Link orchestration for the schema layer. Step 1 carries only the stub that
// makes the build wiring testable; the real pass (edge binding, visibility
// closures, indexing, per-file resolution) arrives in the later plan-4 steps.

#include "schema_linker.hpp"

namespace easypb_schema {

bool link_schema(SchemaSet& files, LinkMode mode,
                 LinkedSchema& result, Diagnostic& error)
{
    (void)files;
    (void)mode;
    (void)result;

    error.code = DIAGNOSTIC_GENERIC;
    error.message = "link_schema: not implemented";
    return false;
}

} // namespace easypb_schema
