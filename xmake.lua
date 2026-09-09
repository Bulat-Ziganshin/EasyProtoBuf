set_languages("c++11")
add_rules("mode.debug", "mode.release")
add_includedirs("include", {public = true})
set_warnings("all", "extra", "pedantic")

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
    add_files("codegen/schema.cpp", "codegen/schema_semantics.cpp")
target_end()

if has_config("codegen_parser") then
    target("easypb_proto_parser")
        set_kind("static")
        add_includedirs("codegen", "codegen/parser", {public = true})
        add_files("codegen/parser/proto_parser.cpp")
        add_deps("easypb_schema")
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
