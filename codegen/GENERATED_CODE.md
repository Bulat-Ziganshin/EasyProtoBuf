# Generated C++ code

EasyProtoBuf Codegen generates plain C++ structures and fixed-`::int32_t` unscoped enum declarations followed by free codec overloads.

Generated output contains `#pragma once`, so it may be included from multiple translation units. The codec functions are emitted as `inline` functions.

## Codec lookup

The codec overloads are found through ADL (argument-dependent lookup), so they must be defined either in the same namespace as the message type or in `easypb`. See [Using the API](../README.md#using-the-api) for details.

## Packages and C++ namespaces

A Protobuf package is emitted as nested C++ namespaces. For example:

```proto
package foo.bar;

message Request {}
```

generates `foo::bar::Request`, with the message declaration and its free `encode`/`decode` overloads inside `namespace foo { namespace bar { ... } }`. A schema without a package is emitted in the global namespace, which already holds the `easypb` runtime and the C++ standard library: a top-level message or enum named `easypb` or `std` is refused there, while inside a package both are ordinary type names. Includes and `#pragma once` remain outside package namespaces.

Generated references to user-defined message and enum types use absolute C++ names such as `::foo::bar::Outer::Inner`. This prevents a nearer C++ declaration from changing which Protobuf type a field names. Package components are emitted verbatim and must therefore be valid, non-reserved C++ namespace identifiers; Codegen rejects C++ keywords, reserved identifier forms, and a package whose first component is `std`.

Imported definitions are never emitted inside the target's header. Their fully qualified Protobuf names map to absolute C++ names, so `.other.Type` becomes `::other::Type`, and the imported type's own generated header provides the definition. Generate the dependencies as well, or otherwise provide matching declarations, before compiling the target.

## Includes, provenance and dependency headers

Every generated header starts with its provenance line - `// Source: <logical name>` - followed by `#pragma once`, the fixed runtime includes, and one `#include "<dependency>.pb.hpp"` per direct dependency in declaration order (public and weak imports are ordinary direct dependencies here). This holds in both link modes: a thin single-file descriptor names its imports even when it does not carry their declarations, and the header it generates still includes them. The anonymous single-file `.pbs` compatibility path uses its container spelling as provenance.

Two compiler-support imports are special-cased: `easypb/options.proto` and `google/protobuf/descriptor.proto` produce no dependency include while they are only option metadata. The exception ends — and the include appears — when a field names a declaration that file owns, or when the import is public, because a re-export must stay usable for the including translation unit. These files always stay in the linked set, so a missing one is still a link error.

The thin single-file descriptor compatibility mode keeps the external-declaration tolerance, but not a different include contract: it emits the same includes as the complete link, and its header is byte-identical to the header the complete closure produces for the same schema. The tolerance covers the external type, not a default its declared kind decides: a message field with a default is refused, and an explicit enum default must be an identifier. An implicit enum default cannot be read from a thin set, so it is the definition-independent zero spelling (`static_cast<EnumType>(0)`); the complete link uses the same spelling for an enum declared in another file whenever that enum's first value is zero, which is what keeps the two headers identical. A cross-file enum whose first value is not zero is the one shape the modes spell differently: the complete link writes `EnumType::FIRST_VALUE`, the thin header can only write the zero value. See [Enums](#enums).

## Nested messages and declaration ordering

Nested Protobuf messages become nested C++ structures:

```proto
message Outer {
  message Inner {
    int32 value = 1;
  }
  Inner inner = 1;
}
```

generates `Outer::Inner` as a nested C++ type.

Acyclic by-value message dependencies are ordered automatically, including forward references between top-level messages and nested siblings.

### Self-recursive containers

By default, recursive message-value graphs are rejected because Codegen emits message fields and containers as C++ values and does not define an ownership/pointer policy. With [`--allow-self-recursive-containers`](OPTIONS.md#structural-options), a message may directly contain itself through a repeated field or as the value of a map. Codegen then emits the ordinary configured C++ container, for example `std::vector<Node>` or `std::map<std::string, Node>`.

The selected C++ container and standard-library implementation must support the incomplete contained type. The C++ standard formally guarantees this for `std::vector` starting with C++17, but the facility predates C++17 in the implementations supported by EasyProtoBuf. In particular, Codegen's own `DescriptorProto` contains `std::vector<DescriptorProto> nested_type`; every CI configuration that builds Codegen therefore already compiles the same self-recursive `std::vector<T>` pattern, including the project's C++11 GCC and Clang/libstdc++ configurations and its pre-C++17 MSVC configurations. The default repeated representation is consequently part of the project's tested C++11 compatibility surface.

Recursive `std::map<K, T>` has no equivalent standard guarantee and is tested separately when the active library accepts it. Custom `--repeated-type` and `--map-type` settings are emitted as requested; Codegen cannot prove that an arbitrary user-selected container supports an incomplete contained or mapped type.

Singular self-recursion, mutual recursion, ancestor recursion, and other recursive message dependency graphs remain rejected. This is a Codegen restriction, not a parser restriction. The container portability responsibility, together with this deliberately narrow recursion scope, is why the feature remains opt-in.

## Enums

Enum fields and map values use generated unscoped C++ enum types with a fixed `::int32_t` underlying type, allowing open proto3 enums to retain unknown wire values.

Top-level enums are emitted before messages, while nested enums are emitted in their owning structure before fields and nested messages that use them. Aliases and negative values are preserved.

Singular fields using an enum declared in the selected file are initialized to their explicit schema default when present, or to the enum's first declared value otherwise. Their initializers use `EnumType::VALUE` qualification, which is valid for unscoped enums in C++11 and prevents nearer enumerators from shadowing the intended value.

An enum declared in another file is spelled by its value, not by its definition, whenever the two agree: an implicit default whose first declared value is zero is emitted as `static_cast<EnumType>(0)`, so the header does not depend on whether the descriptor set carries that file. A non-zero first value keeps `EnumType::FIRST_VALUE` in a complete link.

For an enum whose definition is absent from the descriptor set, Codegen preserves an explicit default when available. Without an explicit default it emits `static_cast<EnumType>(0)`: the definition-independent initializer, so a default-initialized message never carries an indeterminate enumerator, and the compatibility-mode header stays byte-identical to the complete closure's. `--no-default-values` also removes an explicit imported-enum default.

## Presence, required fields, and defaults

By default, Codegen emits `has_*` members for non-repeated fields and uses them when decoding fields whose presence matters. The generated decoder checks the presence of proto2 required fields after decoding and throws `easypb::missing_required_field` if a required field was absent.

These behaviors can be changed with [`--no-has-fields`](OPTIONS.md#structural-options), [`--no-required`](OPTIONS.md#structural-options), and [`--no-default-values`](OPTIONS.md#structural-options).

## Packed repeated fields

Eligible repeated numeric fields can be encoded in packed or unpacked form. [`--packed`](OPTIONS.md#packed-repeated-fields) and [`--no-packed`](OPTIONS.md#packed-repeated-fields) override the schema/default behavior for generated encoders.

## Adapting existing C++ types

`--no-class` suppresses generated structures and enum declarations while leaving the codec overloads available. This can be used to adapt existing C++ types: declare the required message and enum types first, then include the generated output containing only the external codec overloads. For packaged schemas, those handwritten types must be declared in the mapped package namespace.

## Base C++ type generation

Before applying a field/container template, Codegen determines the base C++ type corresponding to each singular Protobuf type:

| Protobuf type | Base C++ type |
|---|---|
| `int32`, `sint32`, `sfixed32` | `::int32_t` |
| `int64`, `sint64`, `sfixed64` | `::int64_t` |
| `uint32`, `fixed32` | `::uint32_t` |
| `uint64`, `fixed64` | `::uint64_t` |
| `double` | `double` |
| `float` | `float` |
| `bool` | `bool` |
| `string`, `bytes` | `--string-type`, `::std::string` by default |
| enum | generated C++ enum type |
| message | generated C++ message type |

For enum and message types, Codegen starts from the fully qualified Protobuf type name, preserves lexical nesting, and emits an absolute C++ reference. Thus `.pkg.Outer.Inner` becomes `::pkg::Outer::Inner`, while `.other.Type` becomes `::other::Type`. See [Packages and C++ namespaces](#packages-and-c-namespaces) and [Nested messages and declaration ordering](#nested-messages-and-declaration-ordering).

This base type is used directly by ordinary singular fields and as the component type substituted into repeated and map field templates.

## C++ field type generation

To use a per-field C++ type in a schema that is also processed by `protoc`, import the EasyProtoBuf option definition:

```proto
syntax = "proto3";

import "easypb/options.proto";

message Example {
    repeated int32 values = 1
        [(easypb.cpp).type = "SmallVector<{}, 4>"];
}
```

`easypb/options.proto` is distributed as `codegen/easypb/options.proto`, so the `codegen` directory is the include root for `protoc`. For example:

```sh
protoc \
    -I . \
    -I /path/to/EasyProtoBuf/codegen \
    --descriptor_set_out=example.pbs \
    example.proto
```

Adding `--include_imports` is optional: Codegen ingests multi-file sets, and a loaded `easypb/options.proto` stays metadata-only, so it needs no options or descriptor include unless a field names their declarations or the import is public (see [Includes, provenance and dependency headers](#includes-provenance-and-dependency-headers)). The embedded `.proto` parser recognizes the EasyProtoBuf option directly as well.

The C++ type of each field is generated from its Protobuf type using a type template. The template is selected according to the field category:

<table>
<thead>
<tr>
<th>Field category</th>
<th>Built-in template</th>
<th>Global override</th>
<th>Per-field override</th>
</tr>
</thead>
<tbody>
<tr>
<td>singular</td>
<td><code>{0}</code></td>
<td><code>--string-type</code> for <code>string</code> and <code>bytes</code></td>
<td rowspan="3"><code>(easypb.cpp).type</code></td>
</tr>
<tr>
<td>repeated</td>
<td><code>::std::vector&lt;{0}&gt;</code></td>
<td><code>--repeated-type</code></td>
</tr>
<tr>
<td>map</td>
<td><code>::std::map&lt;{0}, {1}&gt;</code></td>
<td><code>--map-type</code></td>
</tr>
</tbody>
</table>

A per-field template takes precedence over the corresponding global or built-in template. Choosing a per-field template for a repeated or map field does not bypass normal generation of its component C++ types: those component types are what get substituted into the selected template.

For a **singular field**, `{0}` is the C++ type that Codegen normally generates for its Protobuf type, as described above. For message fields this is the generated message type with the appropriate qualification for namespaces and nested types.

For a **repeated field**, `{0}` is the C++ type that Codegen would generate for its element as a singular field. For example, a repeated message field uses the generated C++ message type, while a repeated `string` field uses the C++ string type selected by `--string-type` or its default.

For a **map field**, `{0}` and `{1}` are the C++ types that Codegen would generate for the key and value respectively as singular fields.

Codegen supports scalar, enum, and message map values. Message-valued maps may use either top-level or nested message types. Synthetic map-entry messages are descriptor details and are not emitted as user-visible C++ structures.

Unnumbered `{}` placeholders in templates are interpreted in order: the first is `{0}`, the second is `{1}`, and so on.

If no placeholder is present in a command-line option, `<{}>` is appended for `--repeated-type` and `<{0},{1}>` for `--map-type`.

A per-field `(easypb.cpp).type` value is always treated as a complete type template. Unlike the command-line repeated/map options, Codegen does not append template arguments when a per-field value contains no placeholders.

For example:

```proto
message Example {
    message Item {...}

    string name = 1;
    Item item = 2;
    repeated string aliases = 3;
    repeated Item items = 4;
    map<string, Item> items_by_name = 5;
}
```

With the default settings, Codegen generates:

```cpp
    ::std::string name;
    ::Example::Item item;
    ::std::vector< ::std::string> aliases;
    ::std::vector< ::Example::Item> items;
    ::std::map< ::std::string, ::Example::Item> items_by_name;
```

Using command-line type templates:

```sh
codegen \
    --string-type 'MyString' \
    --repeated-type 'SmallVector<{}>' \
    --map-type 'FlatMap' \
    example.proto
```

produces:

```cpp
    MyString name;
    ::Example::Item item;
    SmallVector<MyString> aliases;
    SmallVector< ::Example::Item> items;
    FlatMap<MyString, ::Example::Item> items_by_name;
```

Individual fields can override these templates:

```proto
    string name = 1                      [(easypb.cpp).type = "FixedString<64>"];
    Item item = 2                        [(easypb.cpp).type = "std::unique_ptr<{}>"];
    repeated string aliases = 3          [(easypb.cpp).type = "SmallVector<{}, 4>"];
    repeated Item items = 4              [(easypb.cpp).type = "std::deque<{}>"];
    map<string, Item> items_by_name = 5  [(easypb.cpp).type = "FlatMap<{}, {}>"];
```

With the default settings, this generates:

```cpp
    FixedString<64> name;
    std::unique_ptr< ::Example::Item> item;
    SmallVector< ::std::string, 4> aliases;
    std::deque< ::Example::Item> items;
    FlatMap< ::std::string, ::Example::Item> items_by_name;
```

The selected C++ type does not change the Protobuf field type or the generated codec. Custom types therefore still have to support the operations required by the corresponding EasyProtoBuf encoder and decoder.

## Code insertion points

For each generated message type `{TYPE}`, Codegen recognizes four optional insertion macros. Their naming mechanism is unchanged by package namespaces: package components are **not** added to the macro name. For nested messages, `{TYPE}` is the lexical message name with nesting flattened by `_`; for example `Outer::Inner` uses `EASYPB_Outer_Inner_*`, including when `Outer` belongs to a Protobuf package. Consequently, identically named message paths in different packages share the same insertion-macro names, and the flattening is not injective: the top-level `A_B` and the nested `A::B` both use `EASYPB_A_B_*`. This is a documented compatibility artifact (the historical spelling is not versioned); it is accepted and pinned by `codegen.names.insertion_stem_collision`, not treated as a name collision.

```cpp
EASYPB_{TYPE}_EXTRA_FIELDS
EASYPB_{TYPE}_EXTRA_ENCODING(pb, message)
EASYPB_{TYPE}_EXTRA_DECODING(pb, message)
EASYPB_{TYPE}_EXTRA_POST_DECODING(pb, message)
```

`pb` is the current `easypb::Encoder` or `easypb::Decoder`, and `message` is the message object. For example:

```cpp
#define EASYPB_Message_EXTRA_FIELDS \
    bool extra_flag = false;

#define EASYPB_Message_EXTRA_ENCODING(pb, message) \
    (pb).put_bool(100, (message).extra_flag);

#define EASYPB_Message_EXTRA_DECODING(pb, message) \
    case 100: (pb).get_bool(&(message).extra_flag); break;
```

Define insertion macros before including the generated header. See the [Tutorial](../examples/tutorial).

## Current limitations and semantic differences

- `oneof` declarations are parsed, but their alternatives are generated as ordinary fields. Codegen does not enforce mutual exclusion or generate a case discriminator.
- A schema that nests messages deeper than 31 levels is refused with *"Reached maximum recursion limit for nested messages."* — the reference compiler's own limit and its own sentence (`MaxMessageDeclarationNestingDepth()` is 32, and it refuses the 32nd level). The bound holds on both input paths, and it is a bound on the *input*: a descriptor set that nests deeper is refused before it is decoded.
- Declared `group` fields are not supported by Codegen. The EasyProtoBuf decoder can skip unknown groups encountered on the wire, but Codegen does not generate group fields or encoders.
- The C++ scope check enumerates the occupants a generated scope will hold, not the Protobuf declarations it reads. A message's scope holds its fields, its presence flags, its nested types and the enumerators of its nested enums (an unscoped enum's values are members of the enclosing struct); a file's scope holds the top-level types, their enumerators, the `encode`/`decode` overloads the options emit, and - when there is no package - the `easypb` and `std` namespaces. Two occupants that would carry the same C++ name are refused with one diagnostic that names both: a presence flag that collides with a member of its own message, an enumerator that collides with a field, a flag, another enumerator or a nested type, two declarations whose escaped names are equal (`class` and `class_` both become `class_`), a top-level type named like a generated codec, and a top-level type named like a runtime namespace. The reference compiler accepts some of these shapes and emits a header that does not compile — Codegen refuses them instead. The option that owns an occupant removes it from the check too: `--no-has-fields` releases a flag collision, and `--no-encoder`/`--no-decoder` release the matching codec name. A descriptor-origin name that is not a C++ identifier at all — a name starting with a digit, for example - cannot be repaired by the escape rule and is refused for the same reason.
- The embedded parser validates and retains `service` and `rpc` declarations: a method's name and both endpoint types are kept and linked, so a duplicate method name or an endpoint that is missing or is not a message is refused. Method options are validated and discarded, and no RPC client/server API is generated. The parser also validates `extend` declarations but does not retain them in the trimmed descriptor model, and no extension APIs are generated.
- Recursive message-value graphs are rejected by default because generated message fields and containers store values directly. Direct self-recursion through a repeated field or map value can be enabled explicitly with `--allow-self-recursive-containers`; other recursive message dependency graphs remain rejected. See [Self-recursive containers](#self-recursive-containers).
- Generated proto2 required-field checks run at the end of every generated `decode` call rather than once after the complete message has been merged and recursively validated. A singular embedded message whose required fields are split across multiple wire occurrences can therefore be rejected too early. The same issue affects multiple message-value occurrences inside one map entry.
- Conversely, an omitted message value in a map entry creates a default-initialized mapped object without recursively checking its required fields.
- A missing proto2 enum map value is value-initialized to zero rather than using the enum's first declared value, and closed-enum validation semantics are not enforced.
- Unknown fields in ordinary messages, including message values stored in maps, are discarded rather than retained for later serialization.
