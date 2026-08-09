#include <iostream>
#include <string>

#include "maps.generated.hpp"


int main()
{
    // Exercise scalar, byte-string and packed repeated fields together.
    ScalarMaps scalar_source;
    scalar_source.counts["one"] = 1;
    scalar_source.counts["two"] = 2;
    scalar_source.payloads[7] = "payload";
    scalar_source.samples.push_back(10);
    scalar_source.samples.push_back(20);
    scalar_source.samples.push_back(30);

    const std::string scalar_data = easypb::encode(scalar_source);
    const ScalarMaps scalar_copy = easypb::decode<ScalarMaps>(scalar_data);

    if (scalar_copy.counts != scalar_source.counts) {
        std::cerr << "Decoded scalar map differs from the source\n";
        return 1;
    }
    if (scalar_copy.payloads != scalar_source.payloads) {
        std::cerr << "Decoded bytes map differs from the source\n";
        return 1;
    }
    if (scalar_copy.samples != scalar_source.samples) {
        std::cerr << "Decoded packed field differs from the source\n";
        return 1;
    }

    // Enum fields use int32_t, so preserve their numeric values in maps.
    EnumMap enum_source;
    enum_source.statuses["unspecified"] = 0;
    enum_source.statuses["ready"] = 1;

    const std::string enum_data = easypb::encode(enum_source);
    const EnumMap enum_copy = easypb::decode<EnumMap>(enum_data);

    if (enum_copy.statuses != enum_source.statuses) {
        std::cerr << "Decoded enum map differs from the source\n";
        return 1;
    }

    return 0;
}
