# Rules outside the PEG files

[`easypb-parser.peg`](easypb-parser.peg) and [`protobuf-proto2-proto3-complete.peg`](protobuf-proto2-proto3-complete.peg) describe lexical forms and syntactic structure. This document
collects rules that require decoded values, the selected proto syntax,
descriptor types, or symbol tables.

## Lexical and syntactic normalizations

The grammar uses the intended lexical and syntactic forms directly:

- identifiers may begin with an ASCII letter or underscore and may continue
  with letters, digits, or underscores;
- integer and floating tokens are unsigned, while one optional sign is applied
  by productions that permit signed values;
- an exponent has its own optional sign followed by decimal digits;
- `\u` contains four hexadecimal digits and `\U` contains eight;
- adjacent quoted strings form one string literal;
- single- and double-quoted strings exclude their own closing quote, newlines,
  NUL, and an unescaped backslash;
- line comments, block comments, and all whitespace handled by `protoc` are
  represented explicitly;
- proto2 ordinary fields have a label, while oneof fields do not;
- proto3 ordinary fields are unlabeled, `optional`, or `repeated`;
- keyword rules have identifier boundaries;
- message-valued options use the protobuf Text Format aggregate surface.

## Semantic rules implemented by the EasyPB parser

[`../proto_parser.cpp`](../proto_parser.cpp) additionally enforces the following:

- omitted syntax means proto2;
- a syntax declaration must precede every non-empty statement and decode to
  exactly `proto2` or `proto3`;
- at most one package declaration is accepted;
- proto2 ordinary fields require `optional`, `required`, or `repeated`;
- proto3 rejects `required`, explicit defaults, extension ranges, and extend
  declarations;
- proto2 groups are outside the EasyPB parser;
- service/RPC declarations are accepted at top level, including unary and streaming endpoints;
- service and RPC options are validated and consumed but service metadata is not retained;
- map keys must be an allowed scalar key type;
- map and oneof fields reject invalid labels and field options;
- field numbers are 1..536870911, excluding 19000..19999;
- enum values fit signed 32 bits;
- the first value of a non-empty proto3 enum is zero;
- duplicate enum numbers require `option allow_alias = true`;
- enum value names are unique in their containing package or message scope,
  rather than only within each enum declaration;
- duplicate field names and numbers are rejected;
- duplicate type and oneof names are rejected in their scopes;
- reserved and extension ranges must be ordered, disjoint, and conflict-free;
- a field cannot use a reserved name/number or an extension-range number;
- `packed` is valid only for repeated packable primitive or enum fields;
- `FieldOptions.packed` is materialized only when the source explicitly
  contains `[packed = true]` or `[packed = false]`;
- proto3 repeated packable fields are still packed by default on the wire, but
  that effective default is derived from `FileDescriptorProto.syntax` rather than
  stored as an explicit descriptor option;
- defaults are checked against the resolved field type and integer width;
- float and double defaults are stored in the canonical textual form used by
  `protoc`, while string and bytes defaults follow descriptor.proto rules;
- Unicode escapes must decode to Unicode scalar values;
- octal byte escapes may not exceed 255;
- local type names are resolved after parsing and classified as messages or
  enums; unresolved imported names remain `TYPE_MESSAGE` with a warning.

## Parsed syntax versus retained descriptor data

The trimmed descriptor subset cannot retain every source construct.
The EasyPB parser therefore:

- stores imports in `ParsedProto::imports`;
- validates and consumes top-level service/RPC declarations without storing them in the trimmed descriptor model;
- stores proto3 in `FileDescriptorProto.syntax`; proto2 is represented by an absent field 12, matching `protoc`;
- consumes arbitrary options structurally, including parenthesized custom-name
  parts at any dotted position and leading dots inside parentheses, but
  materializes only fields present in the trimmed descriptor model, including
  `default`, `packed`, `allow_alias`, map-entry information, and oneof indices;
- parses proto2 `extend` bodies and emits a warning instead of storing extension
  declarations;
- validates reserved and extension ranges without serializing them.

## Additional syntax in the complete grammar

[`protobuf-proto2-proto3-complete.peg`](protobuf-proto2-proto3-complete.peg) additionally describes:

- proto2 groups;
- full top-level and nested extend declarations;
- extension-range options and message-valued declarations;
- structured aggregate/message option values.

These rules document the broader proto2/proto3 language; they are not claims
that the EasyPB parser accepts or retains those constructs.
