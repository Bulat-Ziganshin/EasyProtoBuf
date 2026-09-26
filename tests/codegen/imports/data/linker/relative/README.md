# Relative-reference fixtures (source path)

The seed set is the code-review lane's corpus for `D-14`
(`build/code-review-2d/corpus/files/r0*.proto`, filed as ledger `D-14`/`D-15`):
references whose FIRST component is a package node, exercised on the SOURCE
path - relative names as a parsed file carries them. The registered `.pbs`
corpus cannot witness this class, because descriptor-set input carries
absolute names.

Verdicts are protoc 30.1's, measured on the source path by fix round r5
(`build/plan4/fix-r5/oracle/oracle-summary.txt`: one invocation per root, no
`--include_imports`, every accept re-read through the descriptor-set
validator):

| Root | protoc | linker |
| --- | --- | --- |
| `r01_nopkg_to_subpkg.proto` | ACCEPT (re-read exit 0) | accept |
| `r03_same_pkg.proto` | ACCEPT (re-read exit 0) | accept |
| `r05_partial_visible.proto` | REJECT `"shared.deep.D" is not defined` | reject, unresolved |
| `r06_pub_import.proto` | ACCEPT (re-read exit 0) | accept |
| `r08_no_import.proto` | REJECT `"a.b.Type" is not defined` | reject, unresolved |

The parser-free suite (`tests/codegen/imports/test_linker.cpp`) carries all
eight seed rows (r01..r08) as in-memory descriptors; this directory carries
the representative five as real source text, so `codegen.imports.source`
exercises the loader -> linker path a user's `codegen file.proto` takes.

Step 8.1 replaces this file with the fixture inventory; until then this file is
the inventory of the `relative/` subtree.
