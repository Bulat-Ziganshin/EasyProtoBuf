#include <iostream>
#include <string>

#include "nested-messages.generated.hpp"
#include "nested-forward.generated.hpp"

int main()
{
    Outer source;
    source.inner.leaf.value = 7;

    Outer::Inner mapped;
    mapped.leaf.value = 11;
    source.by_name["mapped"] = mapped;

    const std::string data = easypb::encode(source);
    const Outer copy = easypb::decode<Outer>(data);
    if (copy.inner.leaf.value != 7 ||
        copy.by_name.at("mapped").leaf.value != 11)
    {
        std::cerr << "Nested message round trip failed\n";
        return 1;
    }

    Holder holder;
    holder.item.leaf.value = 19;
    const Holder holder_copy = easypb::decode<Holder>(easypb::encode(holder));
    if (holder_copy.item.leaf.value != 19) {
        std::cerr << "Qualified nested message round trip failed\n";
        return 1;
    }

    First first;
    first.value.number = 23;
    Parent parent;
    parent.a.value.number = 29;
    if (easypb::decode<First>(easypb::encode(first)).value.number != 23 ||
        easypb::decode<Parent>(easypb::encode(parent)).a.value.number != 29)
    {
        std::cerr << "Forward-reference nested message round trip failed\n";
        return 1;
    }

    return 0;
}
