#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <easypb.hpp>

namespace filetree
{

enum NodeKind : std::uint32_t
{
    regular_file = 1,
    directory = 2,
    symbolic_link = 3,
    other = 4
};

struct Node
{
    std::string_view name;
    std::uint32_t kind = 0;
    std::uint64_t size = 0;
    std::int64_t last_write_time_unix_ns = 0;
    std::uint32_t permissions = 0;
    std::string_view symlink_target;
    std::vector<Node> children;

    bool has_name = false;
    bool has_kind = false;
    bool has_size = false;
    bool has_last_write_time_unix_ns = false;
    bool has_permissions = false;
    bool has_symlink_target = false;
};

inline void encode(easypb::Encoder& pb, const Node& x)
{
    pb.put_string(1, x.name);
    pb.put_fixed32(2, x.kind);

    if (x.has_size)
        pb.put_fixed64(3, x.size);
    if (x.has_last_write_time_unix_ns)
        pb.put_sfixed64(4, x.last_write_time_unix_ns);
    if (x.has_permissions)
        pb.put_fixed32(5, x.permissions);
    if (x.has_symlink_target)
        pb.put_string(6, x.symlink_target);

    pb.put_repeated_message(7, x.children);
}

inline void decode(easypb::Decoder pb, Node& x)
{
    while (pb.get_next_field())
    {
        switch (pb.field_num)
        {
            case 1:
                pb.get_string(&x.name, &x.has_name);
                break;
            case 2:
                pb.get_fixed32(&x.kind, &x.has_kind);
                break;
            case 3:
                pb.get_fixed64(&x.size, &x.has_size);
                break;
            case 4:
                pb.get_sfixed64(
                    &x.last_write_time_unix_ns,
                    &x.has_last_write_time_unix_ns);
                break;
            case 5:
                pb.get_fixed32(&x.permissions, &x.has_permissions);
                break;
            case 6:
                pb.get_string(&x.symlink_target, &x.has_symlink_target);
                break;
            case 7:
                pb.get_repeated_message(&x.children);
                break;
            default:
                pb.skip_field();
        }
    }

    if (!x.has_name)
        throw easypb::missing_required_field(
            "Decoded protobuf has no required field filetree.Node.name");
    if (!x.has_kind)
        throw easypb::missing_required_field(
            "Decoded protobuf has no required field filetree.Node.kind");
}

struct FileTree
{
    Node root;
    bool has_root = false;
};

inline void encode(easypb::Encoder& pb, const FileTree& x)
{
    pb.put_message(1, x.root);
}

inline void decode(easypb::Decoder pb, FileTree& x)
{
    while (pb.get_next_field())
    {
        switch (pb.field_num)
        {
            case 1:
                pb.get_message(&x.root, &x.has_root);
                break;
            default:
                pb.skip_field();
        }
    }

    if (!x.has_root)
        throw easypb::missing_required_field(
            "Decoded protobuf has no required field filetree.FileTree.root");
}

} // namespace filetree
