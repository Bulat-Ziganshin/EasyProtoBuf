#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_map>

using WrappedInt = int32_t;

template <std::size_t N>
using FixedString = std::string;

template <class T>
using MessageBox = T;

template <class T>
using EnumBox = T;

template <class T, std::size_t N>
using SmallVector = std::deque<T>;

using OpaqueRepeated = std::deque<int32_t>;
using OpaqueMap = std::unordered_map<std::string, int32_t>;

#include "custom-types.generated.hpp"

namespace {

bool equal_item(const CustomTypes::Item& a, const CustomTypes::Item& b)
{
    return a.value == b.value;
}

bool equal_message(const CustomTypes& a, const CustomTypes& b)
{
    if (a.ordinary_name != b.ordinary_name ||
        a.ordinary_aliases != b.ordinary_aliases ||
        a.custom_name != b.custom_name ||
        !equal_item(a.item, b.item) ||
        a.aliases != b.aliases ||
        a.items.size() != b.items.size() ||
        a.scalar != b.scalar || a.kind != b.kind ||
        a.opaque_values != b.opaque_values || a.opaque_map != b.opaque_map)
        return false;

    if (a.ordinary_items.size() != b.ordinary_items.size() ||
        a.items_by_name.size() != b.items_by_name.size())
        return false;

    for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!equal_item(a.items[i], b.items[i])) return false;
    }
    for (std::map<std::string, CustomTypes::Item>::const_iterator it = a.ordinary_items.begin();
         it != a.ordinary_items.end(); ++it) {
        std::map<std::string, CustomTypes::Item>::const_iterator found = b.ordinary_items.find(it->first);
        if (found == b.ordinary_items.end() || !equal_item(it->second, found->second)) return false;
    }
    for (std::unordered_map<std::string, CustomTypes::Item>::const_iterator it = a.items_by_name.begin();
         it != a.items_by_name.end(); ++it) {
        std::unordered_map<std::string, CustomTypes::Item>::const_iterator found = b.items_by_name.find(it->first);
        if (found == b.items_by_name.end() || !equal_item(it->second, found->second)) return false;
    }
    return true;
}

} // namespace

int main()
{
    CustomTypes input;
    input.ordinary_name = "ordinary";
    input.ordinary_aliases.push_back("a");
    input.ordinary_aliases.push_back("b");
    input.ordinary_items["first"].value = 11;
    input.custom_name = "custom";
    input.item.value = 22;
    input.aliases.push_back("x");
    input.aliases.push_back("y");
    CustomTypes::Item item;
    item.value = 33;
    input.items.push_back(item);
    input.items_by_name["second"].value = 44;
    input.scalar = 55;
    input.kind = CustomTypes::Kind::ONE;
    input.opaque_values.push_back(66);
    input.opaque_map["third"] = 77;

    const std::string wire = easypb::encode(input);
    const CustomTypes output = easypb::decode<CustomTypes>(wire);
    if (!equal_message(input, output)) {
        std::cerr << "custom type round trip mismatch\n";
        return 1;
    }
    return 0;
}
