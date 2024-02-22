const char* USAGE =
"Schema-less decoder of arbitrary ProtoBuf messages\n"
"  Usage: decoder file.pbs\n";

#include <string>
#include <cctype>
#include <iostream>
#include <fstream>
#include <iterator>

#include <easypb.hpp>


// True if we believe that str contains printable text
bool is_printable_str(easypb::string_view str)
{
    if (str.size() > 100) {
        return false;
    }

    for (auto c: str) {
        if (! isprint(c)) {
           return false;
        }
    }

    return true;
}


// Recursively called printer of the message, contained in str
bool decoder(easypb::string_view str, int indent = 0)
{
    easypb::Decoder pb(str);

    while(pb.get_next_field())
    {
        switch(pb.wire_type)
        {
            case easypb::WIRETYPE_LENGTH_DELIMITED:
            {
                auto str = pb.parse_bytearray_value();
                bool is_printable = is_printable_str(str);

                printf("%*s#%ju: STRING[%zu]%s%.*s\n",
                    indent, "",
                    uintmax_t(pb.field_num),
                    str.size(),
                    (is_printable? " = ": ""),
                    int(is_printable? str.size() : 0),
                    str.data());

                if (! is_printable) {
                    try {
                        decoder(str, indent+4);
                    } catch (const std::exception&) {
                    }
                }
                break;
            }

            case easypb::WIRETYPE_VARINT:
            case easypb::WIRETYPE_FIXED64:
            case easypb::WIRETYPE_FIXED32:
            {
                const char* str_type =
                    (pb.wire_type == easypb::WIRETYPE_FIXED64? "I64" :
                     pb.wire_type == easypb::WIRETYPE_FIXED32? "I32" :
                     "VARINT"
                    );
                int64_t value = pb.parse_integer_value();
                printf("%*s#%ju: %s = %jd\n", indent, "", uintmax_t(pb.field_num), str_type, intmax_t(value));
                break;
            }

            default:  return false;
        }
    }

    return true;
}


int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("%s", USAGE);
        return 1;
    }

    std::ifstream ifs(argv[1], std::ios::binary);
    std::string str(std::istreambuf_iterator<char>{ifs}, {});

    try {
        printf("=== Filesize = %zu\n", str.size());
        decoder(str);
        printf("=== Decoding succeed!\n");
    } catch (const std::exception& e) {
        printf("Internal error: %s\n", e.what());
        return 2;
    }

    return 0;
}
