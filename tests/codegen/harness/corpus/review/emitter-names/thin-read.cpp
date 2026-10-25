#include <cstdio>
#include <cstring>
#include <new>
#include "b.pb.hpp"
int main()
{
    alignas(::p::M) unsigned char buf[sizeof(::p::M)];
    std::memset(buf, 0xAB, sizeof buf);
    ::p::M* m = new (buf) ::p::M;
    std::printf("sizeof=%u e=%d has_e=%d\n", (unsigned)sizeof(::p::M), (int)m->e, (int)m->has_e);
    return 0;
}
