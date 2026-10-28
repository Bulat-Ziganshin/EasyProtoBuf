# The scope-occupant family of the names lane, as data.
#
# CR-03, CR-04 and CR-09 are one rule: the C++ scope check has to enumerate the
# occupants a generated scope will hold, not the instances the Protobuf names
# happen to spell. The lists below are the family: every occupant kind (field,
# presence flag, nested type, enumerator, generated codec, runtime namespace,
# the enclosing class's own name, runtime declaration, global name) crossed with
# the collision pairs that can exist between them, both syntaxes and the options
# that remove an occupant.
#
# THIS FILE IS SHARED. `tests/CMakeLists.txt` includes it at configure time to
# generate one compiled header per accepted variant; `test_names.cmake`
# includes it when the lane runs, to drive both the accepted and the refused
# cases. Adding a case here adds it to the compile matrix and to the driver at
# once, which is the point: the two halves cannot drift apart.
#
# `codegen_names_accept_variants` - entries `fixture|tag|options`:
#   - `tag` names the generated header (`<fixture>.<tag>.pb.hpp`);
#   - `-` is the default option set;
#   - the case must exit 0, leave stderr empty, emit a header, produce the
#     same bytes from the `.proto` and the `.pbs`, and compile.
#
# `codegen_names_refusal_cases` - entries `fixture|options|needles`:
#   - `-` is the default option set;
#   - `>` separates the needles, and a refusal must carry BOTH occupants, so a
#     diagnostic that names one side only fails the case;
#   - the case must exit non-zero and leave stdout empty, on both input paths
#     when the fixture has source text.
#
# The option matrix is the gating proof: an occupant that an option removes
# must stop being an occupant, and an occupant that no option removes must
# refuse under every option set.

set(codegen_names_accept_variants
    # CR-03: --no-has-fields removes the presence flag, so the enumerator is
    # the only declaration that owns the name.
    "enum-flag-p3|no-has-fields|--no-has-fields"
    "enum-flag-p2|no-has-fields|--no-has-fields"
    # The two older flag shapes (W-09, PC-01) are gated by the same option;
    # the family keeps them in the same compile matrix.
    "nested-flag|no-has-fields|--no-has-fields"
    "has-collision|no-has-fields|--no-has-fields"
    "has-collision-p3|no-has-fields|--no-has-fields"
    # CR-04: --no-encoder / --no-decoder remove the codec that owns the name.
    "codec-message-encode|no-encoder|--no-encoder"
    "codec-message-encode-p2|no-encoder|--no-encoder"
    "codec-message-decode|no-decoder|--no-decoder"
    "codec-enum-value-encode|no-encoder|--no-encoder"
    "codec-enum-value-decode|no-decoder|--no-decoder"
    # CR-09 control: the two runtime names are legal inside a package, every
    # runtime reference in the header is absolute, and the header compiles.
    "namespace-packaged|default|-"
    # U-02 controls: `package easypb;` itself is allowed (owner decision #1).
    # The declarations carry no runtime name, so the header compiles next to
    # <easypb.hpp>; `encoder` differs from `Encoder` by case alone, and C++ is
    # case-sensitive, so it is not a collision either.
    "package-easypb|default|-"
    "package-easypb-case|default|-"
    # CR-44 is accepted: both messages are generated and share one insertion
    # stem. The artifact is asserted by
    # `codegen.names.insertion_stem_collision`; this entry only compiles it.
    "insertion-stem-collision|default|-"
    # V-02 controls: the reserved-form rule is a file-scope rule, so a
    # package-local `_Foo` nested inside a message keeps its name and must
    # compile; a top-level `_foo` inside a package does not carry a reserved
    # form either (a leading underscore is reserved only in the global
    # namespace, and `_foo` is not `_` + uppercase).
    "nested-underscore-upper|default|-"
    "leading-underscore-package|default|-")

# Refused under every option set: both occupants are always declared, so no
# option can unlock the fixture.
set(codegen_names_always_refused
    # The older escape-rule rows, kept in the same table so one loop owns the
    # whole family.
    "escape-field|field 'class'>field 'class_'"
    "escape-message|message 'class'>message 'class_'"
    "escape-enum-message|enum 'class'>message 'class_'"
    "escape-enum-value|Enum name class_>same name as class"
    # CR-03: a field, another enumerator, or a nested type.
    "enum-field-p3|field 'class_'>enum value 'class'"
    "enum-field-p2|field 'class_'>enum value 'class'"
    "enum-enum-p3|enum value 'class'>enum value 'class_'"
    "enum-enum-p2|enum value 'class'>enum value 'class_'"
    "nested-enum-p3|enum value 'class'>nested message 'class_'"
    # CR-09: the runtime namespaces are referenced by every header, and no
    # option removes the reference.
    "namespace-easypb|message 'easypb'>runtime namespace 'easypb'"
    "namespace-std|message 'std'>standard library namespace 'std'"
    "namespace-enum-easypb|enum 'easypb'>runtime namespace 'easypb'"
    # U-01: the class's own name is an occupant of its scope. A nested message,
    # a nested enum or an enumerator that carries it is ill-formed in C++
    # ("'Outer::Outer' has the same name as the class in which it is
    # declared"); the reference compiler accepts all three schemas and renames
    # the declaration to `Outer_Outer`.
    "same-name-message|the message 'Outer'>nested message 'Outer'"
    "same-name-enum|the message 'Outer'>nested enum 'Outer'"
    "same-name-enum-value|the message 'Outer'>enum value 'Outer'"
    # U-02: with `package easypb;` the file scope IS the runtime namespace, so
    # every runtime type the decision names is an occupant. Each name has its
    # own fixture, and no generation option removes a runtime declaration.
    "runtime-encoder|message 'Encoder'>runtime type 'Encoder'"
    "runtime-decoder|message 'Decoder'>runtime type 'Decoder'"
    "runtime-writer|message 'Writer'>runtime type 'Writer'"
    "runtime-reader|message 'Reader'>runtime type 'Reader'"
    "runtime-wiretype|message 'WireType'>runtime type 'WireType'"
    "runtime-pbtype|message 'PBType'>runtime type 'PBType'"
    "runtime-stringview|message 'string_view'>runtime type 'string_view'"
    # V-01: the list is the COMPLETE set of declarations `namespace easypb`
    # holds, one fixture per declaration kind the previous seven types missed:
    # an exception type, the `detail` namespace, an anonymous-enum constant, a
    # typed-enum enumerator, a function template and a class template. No
    # generation option removes a runtime declaration.
    "runtime-exception|message 'exception'>runtime type 'exception'"
    "runtime-detail|message 'detail'>runtime namespace 'detail'"
    "runtime-max-varint-size|message 'MAX_VARINT_SIZE'>runtime constant 'MAX_VARINT_SIZE'"
    "runtime-wiretype-varint|message 'WIRETYPE_VARINT'>runtime constant 'WIRETYPE_VARINT'"
    "runtime-read-from-little-endian|message 'read_from_little_endian'>runtime function 'read_from_little_endian'"
    "runtime-pb-type-codec|message 'pb_type_codec'>runtime type 'pb_type_codec'"
    # V-02: the preamble's C library names and the libstdc++ namespace are
    # occupants of an un-packaged file scope, like the R01 typedefs.
    "global-gnu-cxx|message '__gnu_cxx'>C++ library namespace '__gnu_cxx'"
    "global-memcpy|message 'memcpy'>C library function 'memcpy'"
    # V-02: [lex.name] reserves `__` and `_Upper` in every scope; a top-level
    # declaration that carries one is refused with the name and the rule.
    "reserved-underscore-upper|message '_Foo'>reserved C++ identifier form>begins with an underscore followed by an uppercase letter"
    "reserved-double-underscore|message 'foo__bar'>reserved C++ identifier form>contains a double underscore"
    # U-03: the global typedefs the preamble declares are occupants of the
    # global namespace: the header spells every scalar type absolutely, so
    # `struct int32_t` redefines a typedef it uses.
    "global-int32|message 'int32_t'>global typedef 'int32_t'"
    "global-uint64|message 'uint64_t'>global typedef 'uint64_t'"
    "global-size-t|message 'size_t'>global typedef 'size_t'"
    "global-max-align-t|message 'max_align_t'>global typedef 'max_align_t'"
    # CR-10: a descriptor-origin name that is not a C++ identifier is refused
    # rather than emitted; the two needles are the rule and the offending name.
    "digit-field|not a valid C++ identifier>0Zc"
    "digit-value|not a valid C++ identifier>0Xb"
    "digit-message|not a valid C++ identifier>0M"
    "digit-enum|not a valid C++ identifier>0E")

# Refused only while the owner of the name exists. The rows name the options
# that do NOT remove it; the option that does is in `accept_variants`.
set(codegen_names_gated_refused
    # CR-03: the flag is the occupant --no-has-fields removes.
    "enum-flag-p3|--no-encoder|enum value 'has_foo'>presence flag of the field 'foo'"
    "enum-flag-p3|--no-decoder|enum value 'has_foo'>presence flag of the field 'foo'"
    "enum-flag-p2|--no-encoder|enum value 'has_foo'>presence flag of the field 'foo'"
    "enum-flag-p2|--no-decoder|enum value 'has_foo'>presence flag of the field 'foo'"
    "has-collision|-|field 'has_foo'>presence flag of the field 'foo'"
    "has-collision-p3|-|field 'has_foo'>presence flag of the field 'foo'"
    "nested-flag|-|nested message 'has_foo'>presence flag of the field 'foo'"
    # CR-04: only the matching --no-* option removes the codec.
    "codec-message-encode|-|message 'encode'>generated encoder function 'encode'"
    "codec-message-encode|--no-decoder|message 'encode'>generated encoder function 'encode'"
    "codec-message-encode-p2|-|message 'encode'>generated encoder function 'encode'"
    "codec-message-encode-p2|--no-decoder|message 'encode'>generated encoder function 'encode'"
    "codec-message-decode|-|message 'decode'>generated decoder function 'decode'"
    "codec-message-decode|--no-encoder|message 'decode'>generated decoder function 'decode'"
    "codec-enum-value-encode|-|enum value 'encode'>generated encoder function 'encode'"
    "codec-enum-value-encode|--no-decoder|enum value 'encode'>generated encoder function 'encode'"
    "codec-enum-value-decode|-|enum value 'decode'>generated decoder function 'decode'"
    "codec-enum-value-decode|--no-encoder|enum value 'decode'>generated decoder function 'decode'"
    # V-01: `encode`/`decode` are also runtime functions. Removing the codec
    # removes one of the two occupants, not the name: the runtime entry point
    # still owns it, so the declaration stays refused with that second
    # occupant named.
    "runtime-encode|--no-encoder|message 'encode'>runtime function 'encode'"
    "runtime-decode|--no-decoder|message 'decode'>runtime function 'decode'")

# The option set every "always refused" row is run with. The gated rows above
# say their option explicitly, and the accepted variants say theirs.
set(codegen_names_option_sets "-" "--no-has-fields" "--no-encoder" "--no-decoder")

set(codegen_names_refusal_cases)
foreach(_family_option ${codegen_names_option_sets})
    foreach(_family_row ${codegen_names_always_refused})
        string(REPLACE "|" ";" _family_parts "${_family_row}")
        list(GET _family_parts 0 _family_fixture)
        list(GET _family_parts 1 _family_needles)
        list(APPEND codegen_names_refusal_cases
            "${_family_fixture}|${_family_option}|${_family_needles}")
    endforeach()
endforeach()
list(APPEND codegen_names_refusal_cases ${codegen_names_gated_refused})
