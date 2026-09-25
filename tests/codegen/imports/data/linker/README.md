# Linker source fixtures

Fixtures for `codegen.imports.source`
(`tests/codegen/imports/test_source_linker.cpp`), passed to the test
executable as its positional data directory.

This placeholder exists so the directory is tracked: Git does not record empty
directories, and a directory-only creation would make the intermediate commits
and the `codegen.imports.source` registration inconsistent on a fresh
checkout. Step 8.1 replaces this file with the fixture inventory.
