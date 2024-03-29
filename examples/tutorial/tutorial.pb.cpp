// Generated by a ProtoBuf compiler.  DO NOT EDIT!
// Source: tutorial.pbs

#include <cstdint>
#include <string>
#include <vector>


struct SubMessage
{
    int64_t req_int64 = 0;
    int32_t opt_sint32 = 0;
    uint64_t req_uint64 = 0;
    uint32_t opt_fixed32 = 0;
    float req_float = 0;
    std::string opt_string = "DEFAULT STRING";
    std::vector<int32_t> rep_int32;
    std::vector<uint64_t> rep_uint64;
    std::vector<double> rep_double;

    bool has_req_int64 = false;
    bool has_opt_sint32 = false;
    bool has_req_uint64 = false;
    bool has_opt_fixed32 = false;
    bool has_req_float = false;
    bool has_opt_string = false;

#ifdef EASYPB_SubMessage_EXTRA_FIELDS
EASYPB_SubMessage_EXTRA_FIELDS
#endif
    void encode(easypb::Encoder &pb) const;
    void decode(easypb::Decoder pb);
};

void SubMessage::encode(easypb::Encoder &pb) const
{
    pb.put_int64(1, req_int64);
    pb.put_sint32(2, opt_sint32);
    pb.put_uint64(3, req_uint64);
    pb.put_fixed32(4, opt_fixed32);
    pb.put_float(5, req_float);
    pb.put_string(6, opt_string);
    pb.put_repeated_int32(11, rep_int32);
    pb.put_repeated_uint64(12, rep_uint64);
    pb.put_repeated_double(13, rep_double);

#ifdef EASYPB_SubMessage_EXTRA_ENCODING
EASYPB_SubMessage_EXTRA_ENCODING
#endif
}

void SubMessage::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int64(&req_int64, &has_req_int64); break;
            case 2: pb.get_sint32(&opt_sint32, &has_opt_sint32); break;
            case 3: pb.get_uint64(&req_uint64, &has_req_uint64); break;
            case 4: pb.get_fixed32(&opt_fixed32, &has_opt_fixed32); break;
            case 5: pb.get_float(&req_float, &has_req_float); break;
            case 6: pb.get_string(&opt_string, &has_opt_string); break;
            case 11: pb.get_repeated_int32(&rep_int32); break;
            case 12: pb.get_repeated_uint64(&rep_uint64); break;
            case 13: pb.get_repeated_double(&rep_double); break;

#ifdef EASYPB_SubMessage_EXTRA_DECODING
EASYPB_SubMessage_EXTRA_DECODING
#endif
            default: pb.skip_field();
        }
    }
#ifdef EASYPB_SubMessage_EXTRA_POST_DECODING
EASYPB_SubMessage_EXTRA_POST_DECODING
#endif

    if(! has_req_int64) {
        throw easypb::missing_required_field("Decoded protobuf has no required field SubMessage.req_int64");
    }

    if(! has_req_uint64) {
        throw easypb::missing_required_field("Decoded protobuf has no required field SubMessage.req_uint64");
    }

    if(! has_req_float) {
        throw easypb::missing_required_field("Decoded protobuf has no required field SubMessage.req_float");
    }

}

struct MainMessage
{
    uint32_t opt_uint32 = 0;
    int64_t req_sfixed64 = 0;
    double opt_double = 3.14;
    std::string req_bytes;
    SubMessage req_msg;
    std::vector<int32_t> rep_sint32;
    std::vector<uint64_t> rep_fixed64;
    std::vector<std::string> rep_string;
    std::vector<SubMessage> rep_msg;

    bool has_opt_uint32 = false;
    bool has_req_sfixed64 = false;
    bool has_opt_double = false;
    bool has_req_bytes = false;
    bool has_req_msg = false;

#ifdef EASYPB_MainMessage_EXTRA_FIELDS
EASYPB_MainMessage_EXTRA_FIELDS
#endif
    void encode(easypb::Encoder &pb) const;
    void decode(easypb::Decoder pb);
};

void MainMessage::encode(easypb::Encoder &pb) const
{
    pb.put_uint32(1, opt_uint32);
    pb.put_sfixed64(2, req_sfixed64);
    pb.put_double(3, opt_double);
    pb.put_bytes(4, req_bytes);
    pb.put_message(5, req_msg);
    pb.put_repeated_sint32(11, rep_sint32);
    pb.put_repeated_fixed64(12, rep_fixed64);
    pb.put_repeated_string(13, rep_string);
    pb.put_repeated_message(14, rep_msg);

#ifdef EASYPB_MainMessage_EXTRA_ENCODING
EASYPB_MainMessage_EXTRA_ENCODING
#endif
}

void MainMessage::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_uint32(&opt_uint32, &has_opt_uint32); break;
            case 2: pb.get_sfixed64(&req_sfixed64, &has_req_sfixed64); break;
            case 3: pb.get_double(&opt_double, &has_opt_double); break;
            case 4: pb.get_bytes(&req_bytes, &has_req_bytes); break;
            case 5: pb.get_message(&req_msg, &has_req_msg); break;
            case 11: pb.get_repeated_sint32(&rep_sint32); break;
            case 12: pb.get_repeated_fixed64(&rep_fixed64); break;
            case 13: pb.get_repeated_string(&rep_string); break;
            case 14: pb.get_repeated_message(&rep_msg); break;

#ifdef EASYPB_MainMessage_EXTRA_DECODING
EASYPB_MainMessage_EXTRA_DECODING
#endif
            default: pb.skip_field();
        }
    }
#ifdef EASYPB_MainMessage_EXTRA_POST_DECODING
EASYPB_MainMessage_EXTRA_POST_DECODING
#endif

    if(! has_req_sfixed64) {
        throw easypb::missing_required_field("Decoded protobuf has no required field MainMessage.req_sfixed64");
    }

    if(! has_req_bytes) {
        throw easypb::missing_required_field("Decoded protobuf has no required field MainMessage.req_bytes");
    }

    if(! has_req_msg) {
        throw easypb::missing_required_field("Decoded protobuf has no required field MainMessage.req_msg");
    }

}
