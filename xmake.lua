set_languages("c++11")
add_rules("mode.debug", "mode.release")
add_includedirs("include", {public = true})
set_warnings("all", "extra", "pedantic")

target("decoder")
    set_kind("binary")
    add_files("examples/decoder/decoder.cpp")

target("tutorial")
    set_kind("binary")
    add_files("examples/tutorial/main.cpp")

target("codegen")
    set_kind("binary")
    add_includedirs("3rd-party/popl")
    add_files("codegen/main.cpp")
