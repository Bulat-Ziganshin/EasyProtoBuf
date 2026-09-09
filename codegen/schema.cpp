#include "schema.hpp"

#include <cstring>
#include <stdexcept>

namespace easypb_schema {

StringPool::StringPool() {}

StringPool::~StringPool()
{
    clear();
}

void StringPool::clear()
{
    for (std::size_t i = 0; i < blocks_.size(); ++i) delete[] blocks_[i];
    blocks_.clear();
    capacities_.clear();
    used_.clear();
}

str_view StringPool::save(const std::string& value)
{
    return save(value.data(), value.size());
}

str_view StringPool::save(const char* data, std::size_t size)
{
    const std::size_t need = size + 1;
    if (blocks_.empty() || capacities_.back() - used_.back() < need) {
        std::size_t capacity = blocks_.empty() ? 4096u : capacities_.back() * 2u;
        if (capacity > 1024u * 1024u) capacity = 1024u * 1024u;
        if (capacity < need) capacity = need;
        blocks_.push_back(new char[capacity]);
        capacities_.push_back(capacity);
        used_.push_back(0);
    }

    char* destination = blocks_.back() + used_.back();
    if (size != 0) std::memcpy(destination, data, size);
    destination[size] = '\0';
    used_.back() += need;
    return str_view(destination, size);
}

SchemaFile::SchemaFile() : from_descriptor_set(false) {}

void SchemaFile::clear()
{
    file = FileDescriptorProto();
    imports.clear();
    warnings.clear();
    field_sources.clear();
    declaration_locations.clear();
    edges.clear();
    physical_name.clear();
    from_descriptor_set = false;
    strings.clear();
}

SchemaSet::SchemaSet() {}

SchemaSet::~SchemaSet() {}

SchemaFile& SchemaSet::add_file(const std::string& logical_name)
{
    if (index_.find(logical_name) != index_.end()) {
        throw std::runtime_error("duplicate file name " + logical_name);
    }
    files_.push_back(std::unique_ptr<SchemaFile>(new SchemaFile()));
    SchemaFile& file = *files_.back();
    file.file.name = file.strings.save(logical_name);
    file.file.has_name = true;
    file.physical_name = logical_name;
    index_[logical_name] = &file;
    return file;
}

SchemaFile* SchemaSet::find_file(const std::string& logical_name) const
{
    const std::map<std::string, SchemaFile*>::const_iterator found =
        index_.find(logical_name);
    if (found == index_.end()) return 0;
    return found->second;
}

const std::vector<std::unique_ptr<SchemaFile> >& SchemaSet::files() const
{
    return files_;
}

void SchemaSet::add_target(SchemaFile& file)
{
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (targets_[i] == &file) return;
    }
    targets_.push_back(&file);
}

const std::vector<SchemaFile*>& SchemaSet::targets() const
{
    return targets_;
}

void SchemaSet::clear()
{
    files_.clear();
    index_.clear();
    targets_.clear();
}

namespace {

const DescriptorProto* step_into_message(const DescriptorProto& message,
                                         std::size_t& position,
                                         const DescriptorPath& path)
{
    if (position + 1 >= path.size()) return 0;
    const int field_number = path[position];
    const int index = path[position + 1];
    if (index < 0) return 0;
    const std::size_t element = static_cast<std::size_t>(index);
    position += 2;
    if (field_number == 3) {
        if (element >= message.nested_type.size()) return 0;
        return &message.nested_type[element];
    }
    return 0;
}

} // namespace

const DescriptorProto* find_message(const SchemaFile& file,
                                    const DescriptorPath& path)
{
    if (path.size() < 2 || path.size() % 2 != 0) return 0;
    if (path[0] != 4 || path[1] < 0) return 0;
    const std::size_t top = static_cast<std::size_t>(path[1]);
    if (top >= file.file.message_type.size()) return 0;
    const DescriptorProto* message = &file.file.message_type[top];
    std::size_t position = 2;
    while (position < path.size()) {
        message = step_into_message(*message, position, path);
        if (message == 0) return 0;
    }
    return message;
}

const EnumDescriptorProto* find_enum(const SchemaFile& file,
                                     const DescriptorPath& path)
{
    if (path.size() < 2 || path.size() % 2 != 0) return 0;
    if (path[path.size() - 2] != 4 && path[path.size() - 2] != 5) return 0;
    const int index = path[path.size() - 1];
    if (index < 0) return 0;
    const std::size_t element = static_cast<std::size_t>(index);
    if (path.size() == 2) {
        if (path[0] != 5) return 0;
        if (element >= file.file.enum_type.size()) return 0;
        return &file.file.enum_type[element];
    }
    DescriptorPath parent_path(path.begin(), path.end() - 2);
    const DescriptorProto* parent = find_message(file, parent_path);
    if (parent == 0 || path[path.size() - 2] != 4) return 0;
    if (element >= parent->enum_type.size()) return 0;
    return &parent->enum_type[element];
}

const FieldDescriptorProto* find_field(const SchemaFile& file,
                                       const DescriptorPath& path)
{
    if (path.size() < 2 || path.size() % 2 != 0) return 0;
    if (path[path.size() - 2] != 2) return 0;
    const int index = path[path.size() - 1];
    if (index < 0) return 0;
    const std::size_t element = static_cast<std::size_t>(index);
    DescriptorPath parent_path(path.begin(), path.end() - 2);
    const DescriptorProto* parent = find_message(file, parent_path);
    if (parent == 0) return 0;
    if (element >= parent->field.size()) return 0;
    return &parent->field[element];
}

const FieldSource* find_field_source(const SchemaFile& file,
                                     const DescriptorPath& path)
{
    // Linear scan by design: EasyProtoBuf targets small schemas, so the
    // per-field O(n) lookup keeps the code simple. Revisit with a
    // path-to-index map only if large-file profiles regress.
    for (std::size_t i = 0; i < file.field_sources.size(); ++i) {
        if (file.field_sources[i].path == path) return &file.field_sources[i];
    }
    return 0;
}

} // namespace easypb_schema
