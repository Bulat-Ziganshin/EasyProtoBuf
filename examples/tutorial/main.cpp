// Manual definitions for Protobuf map field, since Codegen don't yet support maps
#define EASYPB_MainMessage_EXTRA_FIELDS     std::map<int,int> mappa;
#define EASYPB_MainMessage_EXTRA_ENCODING   pb.put_map_int32_int32(15, mappa);
#define EASYPB_MainMessage_EXTRA_DECODING   case 15: pb.get_map_int32_int32(&mappa); break;

#include <map>
#include <easypb.hpp>
#include "tutorial.pb.cpp"


// Fill msg with some data
MainMessage make_message()
{
    MainMessage msg;

    msg.opt_uint32   = 101;
    msg.req_sfixed64 = -102;
    msg.opt_double   = 103.14;
    msg.req_bytes    = "104";

    msg.req_msg.req_int64    = -201;
    msg.req_msg.opt_sint32   = -202;
    msg.req_msg.req_uint64   = 203;
    msg.req_msg.opt_fixed32  = 204;
    msg.req_msg.req_float    = -205.42f;
    msg.req_msg.opt_string   = "206";

    msg.mappa[1] = 1234;
    msg.mappa[2] = 4321;

    return msg;
}


// Compare two message records and return the name of the first unequal field found,
// or nullptr if records are equal
const char* compare(MainMessage& msg1, MainMessage& msg2)
{
    if (msg1.opt_uint32   != msg2.opt_uint32  )  return "opt_uint32";
    if (msg1.req_sfixed64 != msg2.req_sfixed64)  return "req_sfixed64";
    if (msg1.opt_double   != msg2.opt_double  )  return "opt_double";
    if (msg1.req_bytes    != msg2.req_bytes   )  return "req_bytes";

    if (msg1.req_msg.req_int64   != msg2.req_msg.req_int64  )  return "req_msg.req_int64";
    if (msg1.req_msg.opt_sint32  != msg2.req_msg.opt_sint32 )  return "req_msg.opt_sint32";
    if (msg1.req_msg.req_uint64  != msg2.req_msg.req_uint64 )  return "req_msg.req_uint64";
    if (msg1.req_msg.opt_fixed32 != msg2.req_msg.opt_fixed32)  return "req_msg.opt_fixed32";
    if (msg1.req_msg.req_float   != msg2.req_msg.req_float  )  return "req_msg.req_float";
    if (msg1.req_msg.opt_string  != msg2.req_msg.opt_string )  return "req_msg.opt_string";

    for(const auto& x : msg1.mappa) {
        if(msg1.mappa[x.first] != msg2.mappa[x.first])  return "mappa";
    }
    for(const auto& x : msg2.mappa) {
        if(msg1.mappa[x.first] != msg2.mappa[x.first])  return "mappa";
    }

    return nullptr;
}


int main()
{
    try {
        MainMessage orig_msg = make_message();

        // Encode message into a string buffer
        std::string buffer = easypb::encode(orig_msg);

        // Decode message from the string buffer
        auto decoded_msg = easypb::decode<MainMessage>(buffer);

        // Check whether the decoded message is the same as the original
        auto error = compare(orig_msg, decoded_msg);
        if (error) {
            printf("Incorrectly restored field: %s\n", error);
            return 1;
        } else {
            printf("Data restored correctly!\n");
        }

    } catch (const std::exception& e) {
        printf("Exception: %s\n", e.what());
        return 2;
    }
    return 0;
}
