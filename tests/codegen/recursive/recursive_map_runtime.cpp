#include <iostream>
#include <string>

#include "recursive-map.generated.hpp"

int main()
{
    MapNode source;
    MapNode child;
    child.children["grandchild"] = MapNode();
    source.children["child"] = child;

    const MapNode copy = easypb::decode<MapNode>(easypb::encode(source));
    if (copy.children.size() != 1 ||
        copy.children.at("child").children.size() != 1 ||
        copy.children.at("child").children.count("grandchild") != 1)
    {
        std::cerr << "Self-recursive map message round trip failed\n";
        return 1;
    }

    return 0;
}
