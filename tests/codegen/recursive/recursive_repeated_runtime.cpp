#include <iostream>

#include "recursive-repeated.generated.hpp"

int main()
{
    RepeatedNode source;
    RepeatedNode child;
    child.children.push_back(RepeatedNode());
    source.children.push_back(child);

    const RepeatedNode copy = easypb::decode<RepeatedNode>(easypb::encode(source));
    if (copy.children.size() != 1 || copy.children[0].children.size() != 1)
    {
        std::cerr << "Self-recursive repeated message round trip failed\n";
        return 1;
    }

    return 0;
}
