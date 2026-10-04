// Plan 5's two-TU runtime proof, first translation unit: it includes both
// dependency headers and the root header together - the generated headers
// must coexist in one translation unit - and round-trips enum values, a
// packed repeated enum, a nested message and a message-valued map through
// the public EasyProtoBuf API. The second translation unit
// (two_tu_shared.cpp) includes the shared dependency directly and provides
// the two functions declared below, so both TUs instantiate the shared
// header's codecs.

#include <iostream>
#include <map>
#include <string>

#include "api/report.pb.hpp"
#include "common/node.pb.hpp"
#include "common/state.pb.hpp"

common::Node make_shared_node(::int32_t value);
bool shared_node_round_trip(::int32_t value);

namespace {

int failures = 0;

void check(bool condition, const char* expression)
{
    if (!condition) {
        std::cerr << "CHECK failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x)

} // namespace

int main()
{
    // The imported proto2 enum's implicit default is the deliberately nonzero
    // READY (7), observable before any assignment.
    api::Report fresh;
    CHECK(fresh.state == shared::State::READY);

    api::Report source;
    source.state = shared::State::STOPPED;
    source.history.push_back(shared::State::READY);
    source.history.push_back(shared::State::STOPPED);
    source.nodes["first"] = make_shared_node(11);
    source.nodes["second"] = make_shared_node(22);
    source.detail.value = 33;

    const std::string data = easypb::encode(source);
    const api::Report copy = easypb::decode<api::Report>(data);

    CHECK(copy.state == shared::State::STOPPED);
    CHECK(copy.history.size() == 2);
    if (copy.history.size() == 2) {
        CHECK(copy.history[0] == shared::State::READY);
        CHECK(copy.history[1] == shared::State::STOPPED);
    }
    CHECK(copy.nodes.size() == 2);
    if (copy.nodes.size() == 2) {
        CHECK(copy.nodes.at("first").data.value == 11);
        CHECK(copy.nodes.at("second").data.value == 22);
    }
    CHECK(copy.detail.value == 33);

    // The shared dependency's codecs, exercised from the other translation
    // unit and through a node built here.
    CHECK(shared_node_round_trip(44));
    const std::string node_data = easypb::encode(make_shared_node(55));
    const common::Node node_copy = easypb::decode<common::Node>(node_data);
    CHECK(node_copy.data.value == 55);

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all runtime integration checks passed\n";
    return 0;
}
