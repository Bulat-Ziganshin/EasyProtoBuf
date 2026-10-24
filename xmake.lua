set_languages("c++11")
add_rules("mode.debug", "mode.release")
add_includedirs("include", {public = true})
set_warnings("all", "extra", "pedantic")

-- P-03: an unhandled enumerator must not compile. See CMakeLists.txt for the
-- full reason; `set_warnings("all")` above only warns, and the guarantee needs
-- the error form. MSVC is left out for the same reason it is left out there: no
-- measured lane for it in this repository, and the codes are not
-- interchangeable - `outcome_code` has no `default:` label, so its analogue is
-- /we4062 (C4062), while /we4061 (C4061) is the one for a default-bearing
-- switch (fix round r5, H-3).
if is_compiler("gcc") or is_compiler("clang") then
    add_cxflags("-Werror=switch")
end

option("codegen_parser")
    set_default(true)
    set_showmenu(true)
    set_description("Build .proto parser, descriptor printer and benchmark into codegen")
option_end()

target("decoder")
    set_kind("binary")
    add_files("examples/decoder/decoder.cpp")

target("tutorial")
    set_kind("binary")
    add_files("examples/tutorial/main.cpp")

target("easypb_schema")
    set_kind("static")
    add_includedirs("codegen", {public = true})
    add_files("codegen/schema.cpp", "codegen/schema_semantics.cpp",
              "codegen/logical_paths.cpp", "codegen/schema_linker.cpp",
              "codegen/descriptor_adapter.cpp")
target_end()

target("easypb_codegen_core")
    set_kind("static")
    add_includedirs("codegen", {public = true})
    add_files("codegen/codegen.cpp", "codegen/cpp_names.cpp",
              "codegen/generation_options.cpp", "codegen/header_paths.cpp",
              "codegen/utils.cpp")
    add_deps("easypb_schema")
target_end()

-- Standalone I/O shared by the full and the lite product: plan 3's physical
-- path rules and the `.pbs` file wrapper. Neither belongs in easypb_schema,
-- which never touches the disk; the source loader above the descriptor loader
-- stays full-only and links easypb_file_io rather than compiling it twice.
-- See CMakeLists.txt for the same split.
target("easypb_file_io")
    set_kind("static")
    add_includedirs("codegen", {public = true})
    add_files("codegen/file_paths.cpp")
target_end()

target("easypb_descriptor_loader")
    set_kind("static")
    add_includedirs("codegen", {public = true})
    add_files("codegen/descriptor_loader.cpp")
    add_deps("easypb_file_io", "easypb_schema")
target_end()

-- Plan 5's standalone publisher: output-name validation, duplicate output-name
-- refusal, directory creation and the in-place write. It is NOT part of the
-- generation core (P11). See CMakeLists.txt for the same split.
target("easypb_header_output")
    set_kind("static")
    add_includedirs("codegen", {public = true})
    add_files("codegen/header_output.cpp")
    add_deps("easypb_file_io", "easypb_schema")
target_end()

if has_config("codegen_parser") then
    target("easypb_proto_parser")
        set_kind("static")
        add_includedirs("codegen", "codegen/parser", {public = true})
        add_files("codegen/parser/proto_parser.cpp")
        add_deps("easypb_schema")
    target_end()

    target("easypb_proto_loader")
        set_kind("static")
        add_includedirs("codegen", "codegen/parser", {public = true})
        add_files("codegen/proto_loader.cpp")
        add_deps("easypb_file_io", "easypb_proto_parser", "easypb_schema")
    target_end()
end

target("codegen")
    set_kind("binary")
    add_includedirs("3rd-party/popl", "codegen")
    add_files("codegen/main.cpp")
    add_deps("easypb_codegen_core", "easypb_descriptor_loader",
             "easypb_header_output")
    if is_plat("windows") then
        -- CommandLineToArgvW before the one UTF-8 conversion (plan 5 C4).
        add_syslinks("shell32")
    end
    if has_config("codegen_parser") then
        add_defines("EASYPB_CODEGEN_WITH_PROTO_PARSER=1")
        add_files("codegen/parser/pretty_printer.cpp",
                  "codegen/parser/parser_benchmark.cpp")
        add_deps("easypb_proto_parser", "easypb_proto_loader")
    else
        add_defines("EASYPB_CODEGEN_WITH_PROTO_PARSER=0")
    end
