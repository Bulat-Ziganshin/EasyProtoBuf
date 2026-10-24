# The scope-occupant family of the names lane, as data.
#
# CR-03, CR-04 and CR-09 are one rule: the C++ scope check has to enumerate the
# occupants a generated scope will hold, not the instances the Protobuf names
# happen to spell. The lists below are the family: every occupant kind (field,
# presence flag, nested type, enumerator, generated codec, runtime namespace)
# crossed with the collision pairs that can exist between them, both syntaxes
# and the three options that remove an occupant.
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
    # CR-44 is accepted: both messages are generated and share one insertion
    # stem. The artifact is asserted by
    # `codegen.names.insertion_stem_collision`; this entry only compiles it.
    "insertion-stem-collision|default|-")

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
    "codec-enum-value-decode|--no-encoder|enum value 'decode'>generated decoder function 'decode'")

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
