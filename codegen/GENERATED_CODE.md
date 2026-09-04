# Generated C++ code

EasyProtoBuf Codegen generates plain C++ structures and fixed-`int32_t` unscoped enum declarations followed by free codec overloads.

Generated output contains `#pragma once`, so it may be included from multiple translation units. The codec functions are emitted as `inline` functions.

## Codec lookup

The codec overloads are found through ADL (argument-dependent lookup), so they must be defined either in the same namespace as the message type or in `easypb`. See [Using the API](../README.md#using-the-api) for details.

## Packages and C++ namespaces

Codegen uses a schema's package when resolving Protobuf type names but does not generate a corresponding C++ namespace. The current file's package prefix is removed, so its top-level messages and enums are emitted in the global C++ namespace. Schemas from different packages can therefore produce colliding top-level C++ names.

Descriptor-set generation does not emit definitions for types imported from another schema. Same-package imported types use the same unqualified mapping as local top-level types, while names from another package become absolute C++ names such as `::other::Type`. Include, declare, or alias the corresponding C++ types before they are needed by the generated header. Because Codegen does not create package namespaces, separately generating the imported schema automatically provides the expected name only for same-package imports; cross-package imports need an application-supplied namespace mapping or adapter.

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

Enum fields and map values use generated unscoped C++ enum types with a fixed `int32_t` underlying type, allowing open proto3 enums to retain unknown wire values.

Top-level enums are emitted before messages, while nested enums are emitted in their owning structure before fields and nested messages that use them. Aliases and negative values are preserved.

Singular fields using an enum declared in the selected file are initialized to their explicit schema default when present, or to the enum's first declared value otherwise. Their initializers use `EnumType::VALUE` qualification, which is valid for unscoped enums in C++11 and prevents nearer enumerators from shadowing the intended value.

For an imported enum whose definition is absent from the descriptor set, Codegen preserves an explicit default when available but cannot infer the first declared value. Without an explicit default, it emits no initializer for that field. `--no-default-values` also removes an explicit imported-enum default.

## Repeated and map fields

By default, repeated fields use `std::vector` and map fields use `std::map`. These container templates can be changed with [`--repeated-type`](OPTIONS.md#c-type-options) and [`--map-type`](OPTIONS.md#c-type-options).

Codegen supports scalar, enum and message map values. Message-valued maps may use either top-level or nested message types. Synthetic map-entry messages are descriptor details and are not emitted as user-visible C++ structures.

## Presence, required fields, and defaults

By default, Codegen emits `has_*` members for non-repeated fields and uses them when decoding fields whose presence matters. The generated decoder checks the presence of proto2 required fields after decoding and throws `easypb::missing_required_field` if a required field was absent.

These behaviors can be changed with [`--no-has-fields`](OPTIONS.md#structural-options), [`--no-required`](OPTIONS.md#structural-options), and [`--no-default-values`](OPTIONS.md#structural-options).

## Packed repeated fields

Eligible repeated numeric fields can be encoded in packed or unpacked form. [`--packed`](OPTIONS.md#packed-repeated-fields) and [`--no-packed`](OPTIONS.md#packed-repeated-fields) override the schema/default behavior for generated encoders.

## Adapting existing C++ types

`--no-class` suppresses generated structures and enum declarations while leaving the codec overloads available. This can be used to adapt existing C++ types: declare the required message and enum types first, then include the generated output containing only the external codec overloads.

The generated field types themselves can also be customized with `--string-type`, `--repeated-type`, and `--map-type`; see [Command-line options](OPTIONS.md#c-type-options).

## Code insertion points

For each generated message type `{TYPE}`, Codegen recognizes four optional insertion macros. For nested messages, `{TYPE}` is the qualified message name with `::` replaced by `_`; for example `Outer::Inner` uses `EASYPB_Outer_Inner_*`.

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
- Declared `group` fields are not supported by Codegen. The EasyProtoBuf decoder can skip unknown groups encountered on the wire, but Codegen does not generate group fields or encoders.
- The embedded parser validates and skips `service` and `rpc` declarations. It also validates `extend` declarations but does not retain them in the trimmed descriptor model. No RPC or extension APIs are generated.
- Recursive message-value graphs are rejected by default because generated message fields and containers store values directly. Direct self-recursion through a repeated field or map value can be enabled explicitly with `--allow-self-recursive-containers`; other recursive message dependency graphs remain rejected. See [Self-recursive containers](#self-recursive-containers).
- Generated proto2 required-field checks run at the end of every generated `decode` call rather than once after the complete message has been merged and recursively validated. A singular embedded message whose required fields are split across multiple wire occurrences can therefore be rejected too early. The same issue affects multiple message-value occurrences inside one map entry.
- Conversely, an omitted message value in a map entry creates a default-initialized mapped object without recursively checking its required fields.
- A missing proto2 enum map value is value-initialized to zero rather than using the enum's first declared value, and closed-enum validation semantics are not enforced.
- Unknown fields in ordinary messages, including message values stored in maps, are discarded rather than retained for later serialization.
