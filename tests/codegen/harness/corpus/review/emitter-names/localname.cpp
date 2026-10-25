#include <cstdio>
#include <string>
#include "cpp_names.hpp"
int main()
{
    printf("[%s]\n", easypb_codegen::cpp_local_name(".pkg.class", "pkg").c_str());
    printf("[%s]\n", easypb_codegen::cpp_local_name(".pkg.Outer.class", "pkg").c_str());
    printf("[%s]\n", easypb_codegen::cpp_absolute_name(".pkg.class").c_str());
    printf("[%s]\n", easypb_codegen::cpp_identifier("class").c_str());
    return 0;
}
