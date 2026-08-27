#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "enums.generated.hpp"


static_assert(std::is_same<decltype(Job().status), Status>::value,
              "singular enum fields must use the generated enum type");
static_assert(std::is_same<decltype(Job().priority), Job::Priority>::value,
              "nested enum fields must use the generated enum type");
static_assert(std::is_same<decltype(Job().history), std::vector<Status> >::value,
              "repeated enum fields must preserve their enum type");
static_assert(std::is_same<decltype(Job().names),
                           std::map<std::string, Status> >::value,
              "enum map values must preserve their enum type");
static_assert(std::is_same<decltype(Job().current), Status>::value,
              "enum fields without defaults must preserve their enum type");
static_assert(std::is_same<decltype(Audit().priority), Job::Priority>::value,
              "forward nested enum fields must use their declared type");
static_assert(std::is_same<decltype(Audit().priorities),
                           std::vector<Job::Priority> >::value,
              "forward repeated nested enum fields must preserve their type");
static_assert(std::is_same<decltype(Audit().priorities_by_name),
                           std::map<std::string, Job::Priority> >::value,
              "forward nested enum map values must preserve their type");
static_assert(std::is_same<decltype(ImplicitDefaults().value), NonZero>::value,
              "implicit enum defaults must preserve their enum type");


int main()
{
    Job defaults;
    Audit audit_defaults;
    ImplicitDefaults implicit_defaults;
    if (defaults.status != STARTED ||
        defaults.priority != Job::HIGH ||
        defaults.current != UNKNOWN ||
        audit_defaults.priority != Job::HIGH ||
        implicit_defaults.value != FIVE)
    {
        std::cerr << "Generated enum defaults are incorrect\n";
        return 1;
    }

    Job source;
    source.status = FAILED;
    source.priority = Job::LOW;
    source.history.push_back(STARTED);
    source.history.push_back(FAILED);
    source.packed_history.push_back(UNKNOWN);
    source.packed_history.push_back(FAILED);
    source.names["active"] = ACTIVE;
    source.names["failed"] = FAILED;
    source.current = ACTIVE;

    const std::string data = easypb::encode(source);

    // Field 1 is encoded first. A canonical int32 -1 varint uses ten bytes;
    // ZigZag encoding would instead produce the one-byte payload 0x01.
    const unsigned char negative_enum_prefix[] = {
        0x08,
        0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    if (data.size() < sizeof(negative_enum_prefix)) {
        std::cerr << "Encoded negative enum is unexpectedly short\n";
        return 1;
    }
    for (std::size_t i = 0; i < sizeof(negative_enum_prefix); ++i) {
        if (static_cast<unsigned char>(data[i]) != negative_enum_prefix[i]) {
            std::cerr << "Negative enum did not use canonical int32 varint encoding\n";
            return 1;
        }
    }

    const Job copy = easypb::decode<Job>(data);
    if (copy.status != source.status ||
        copy.priority != source.priority ||
        copy.history != source.history ||
        copy.packed_history != source.packed_history ||
        copy.names != source.names ||
        copy.current != source.current)
    {
        std::cerr << "Generated enum round trip failed\n";
        return 1;
    }

    Audit audit_source;
    audit_source.priority = Job::HIGH;
    audit_source.priorities.push_back(Job::LOW);
    audit_source.priorities.push_back(Job::HIGH);
    audit_source.priorities_by_name["low"] = Job::LOW;
    audit_source.priorities_by_name["high"] = Job::HIGH;

    const Audit audit_copy = easypb::decode<Audit>(easypb::encode(audit_source));
    if (audit_copy.priority != audit_source.priority ||
        audit_copy.priorities != audit_source.priorities ||
        audit_copy.priorities_by_name != audit_source.priorities_by_name)
    {
        std::cerr << "Forward nested enum round trip failed\n";
        return 1;
    }

    return 0;
}
