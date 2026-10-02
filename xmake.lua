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
    add_files("codegen/schema.cpp", "codegen/schema_semantics.cpp", "codegen/logical_paths.cpp", "codegen/schema_linker.cpp")
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
        add_files("codegen/proto_loader.cpp", "codegen/file_paths.cpp")
        add_deps("easypb_proto_parser", "easypb_schema")
    target_end()
end

target("codegen")
    set_kind("binary")
    add_includedirs("3rd-party/popl", "codegen")
    add_files("codegen/main.cpp", "codegen/cpp_names.cpp")
    add_deps("easypb_schema")
    if has_config("codegen_parser") then
        add_defines("EASYPB_CODEGEN_WITH_PROTO_PARSER=1")
        add_files("codegen/parser/pretty_printer.cpp",
                  "codegen/parser/parser_benchmark.cpp")
        add_deps("easypb_proto_parser")
    else
        add_defines("EASYPB_CODEGEN_WITH_PROTO_PARSER=0")
    end
