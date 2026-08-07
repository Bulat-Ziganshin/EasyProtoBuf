# `.proto` grammar and lexer catalogue

This directory contains two formal descriptions of the Protocol Buffers schema language. Both use PEG notation and include lexical rules in the same file as parser productions.

The notation is compatible with the cpp-peglib 1.9.x grammar format. The EasyPB parser itself does **not** depend on [cpp-peglib](https://github.com/yhirose/cpp-peglib).

| File | Meaning |
|---|---|
| [`easypb-parser.peg`](easypb-parser.peg) | The lexer and syntactic frontend implemented by [`../proto_parser.cpp`](../proto_parser.cpp). |
| [`protobuf-proto2-proto3-complete.peg`](protobuf-proto2-proto3-complete.peg) | A broader implementation-oriented proto2/proto3 grammar, including constructs intentionally outside the EasyPB parser. |

## PEG notation

The files use the normal cpp-peglib operators:

- `A <- B` defines a rule;
- `A / B` is ordered choice;
- `?`, `*`, and `+` mean optional, zero-or-more, and one-or-more;
- `&A` and `!A` are positive and negative lookahead;
- `< A >` marks a token boundary/capture;
- `!.` requires end of input;
- `%whitespace` defines automatically skipped spaces and comments.

The `map` production uses lookahead (`K_MAP &"<"`) deliberately. This mirrors both `protoc` and the EasyPB parser: `map<string, T>` is a map field, while `map value` may use a user-defined type whose name is `map`.

## EasyPB parser grammar

[`easypb-parser.peg`](easypb-parser.peg) describes token spellings and the recursive-descent syntax implemented by [`../proto_parser.cpp`](../proto_parser.cpp). It deliberately separates grammar from checks that use numeric values, the selected proto syntax, descriptor types, or symbol tables. Those checks are documented in [`SEMANTICS.md`](SEMANTICS.md).

The grammar describes what the frontend can parse structurally, not every source construct retained by the trimmed descriptor model. For example, the parser can consume proto2 `extend` declarations and report that they are not represented by the trimmed `FileDescriptorProto`.

## Complete proto2/proto3 grammar

[`protobuf-proto2-proto3-complete.peg`](protobuf-proto2-proto3-complete.peg) is the broader engineering reference. It includes services and RPCs, deprecated groups, full extend syntax, extension-range options, parenthesized custom option-name parts, and structured message-valued options.

“Complete” here means the proto2 and proto3 schema languages. Protobuf Editions are a separate language revision and are outside the current parser scope.

## Formal grammar versus semantic validity

PEG describes tokenization and syntactic structure well, but several protobuf rules require values and symbol information. Examples include field-number limits, duplicate declarations, reserved-range conflicts, proto2/proto3 feature selection, default-value typing, and resolution of message versus enum names.

Those rules are documented in [`SEMANTICS.md`](SEMANTICS.md) and implemented by the semantic pass in [`../proto_parser.cpp`](../proto_parser.cpp).

## Reference sources

- [Proto2 specification](https://protobuf.dev/reference/protobuf/proto2-spec/)
- [Proto3 specification](https://protobuf.dev/reference/protobuf/proto3-spec/)
- [Text Format specification](https://protobuf.dev/reference/protobuf/textformat-spec/)
- [Extension declarations](https://protobuf.dev/programming-guides/extension_declarations/)
- [cpp-peglib](https://github.com/yhirose/cpp-peglib)
