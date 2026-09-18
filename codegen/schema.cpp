#include "schema.hpp"
#include "logical_paths.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace easypb_schema {

namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

std::string importing_name(const SchemaFile& file)
{
    if (file.file.has_name) return view_text(file.file.name);
    return std::string();
}

// Source coordinates come from imports only. When the descriptor list
// and the location-bearing list agree positionally the index is used
// directly; otherwise the first matching path supplies the position so
// hand-built descriptor fixtures still locate their declaration.
SourceLocation import_location(const SchemaFile& file,
                               std::size_t dependency_index,
                               const std::string& dependency)
{
    if (dependency_index < file.imports.size() &&
        file.imports[dependency_index].path == dependency) {
        return file.imports[dependency_index].location;
    }
    for (std::size_t i = 0; i < file.imports.size(); ++i) {
        if (file.imports[i].path == dependency) return file.imports[i].location;
    }
    return unknown_location();
}

std::string modifier_text(ImportInfo::Modifier modifier)
{
    switch (modifier) {
        case ImportInfo::PUBLIC_IMPORT: return "public ";
        case ImportInfo::WEAK_IMPORT: return "weak ";
        default: return "";
    }
}

void clear_all_edges(SchemaSet& files)
{
    const std::vector<std::unique_ptr<SchemaFile> >& owned = files.files();
    for (std::size_t i = 0; i < owned.size(); ++i) owned[i]->edges.clear();
}

bool fail_import(SchemaSet& files, const SchemaFile& importer,
                 const SourceLocation& location, DiagnosticCode code,
                 const std::string& message, Diagnostic& error)
{
    clear_all_edges(files);
    error.file = importing_name(importer);
    error.location = location;
    error.message = message;
    error.warning = false;
    error.code = code;
    return false;
}

std::string cycle_text(const std::vector<SchemaFile*>& chain)
{
    std::ostringstream output;
    output << "import cycle: ";
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i != 0) output << " -> ";
        output << importing_name(*chain[i]);
    }
    return output.str();
}

} // namespace

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

bool bind_import_edges(SchemaSet& files, bool allow_missing,
                       Diagnostic& error)
{
    error = Diagnostic();
    clear_all_edges(files);

    const std::vector<std::unique_ptr<SchemaFile> >& owned = files.files();

    for (std::size_t file_index = 0; file_index < owned.size(); ++file_index) {
        SchemaFile& importer = *owned[file_index];
        const std::string importer_name = importing_name(importer);
        const std::size_t dependency_count = importer.file.dependency.size();

        // Repeated dependency names are rejected before index checks so
        // the duplicate spelling is reported instead of an aliasing edge.
        {
            std::set<std::string> seen;
            for (std::size_t i = 0; i < dependency_count; ++i) {
                const std::string name = view_text(importer.file.dependency[i]);
                if (!seen.insert(name).second) {
                    std::ostringstream message;
                    message << "file \"" << importer_name
                            << "\": duplicate import \"" << name << "\"";
                    return fail_import(files, importer,
                                       import_location(importer, i, name),
                                       DIAGNOSTIC_INVALID_IMPORT,
                                       message.str(), error);
                }
            }
        }

        // Logical spelling is validated identically for source,
        // descriptor-set, and future plugin input.
        for (std::size_t i = 0; i < dependency_count; ++i) {
            const std::string name = view_text(importer.file.dependency[i]);
            std::string reason;
            if (!validate_logical_path(name, reason)) {
                std::ostringstream message;
                message << "file \"" << importer_name << "\": invalid import \""
                        << name << "\": " << reason;
                return fail_import(files, importer,
                                   import_location(importer, i, name),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
        }

        // Index/modifier validation. The descriptor lists are authoritative
        // for graph meaning; imports supplies positions only.
        std::set<std::int32_t> public_indexes;
        std::set<std::int32_t> weak_indexes;
        for (std::size_t i = 0; i < importer.file.public_dependency.size(); ++i) {
            const std::int32_t index = importer.file.public_dependency[i];
            if (index < 0 ||
                static_cast<std::size_t>(index) >= dependency_count) {
                std::ostringstream message;
                message << "file \"" << importer_name
                        << "\": invalid public dependency index " << index
                        << " (dependency count " << dependency_count << ")";
                return fail_import(files, importer, unknown_location(),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
            if (!public_indexes.insert(index).second) {
                // The index is range-checked above, so the dependency and
                // its declaration site are known: keep the position.
                const std::string name =
                    view_text(importer.file.dependency[static_cast<std::size_t>(index)]);
                std::ostringstream message;
                message << "file \"" << importer_name
                        << "\": duplicate public dependency index " << index;
                return fail_import(files, importer,
                                   import_location(importer,
                                                   static_cast<std::size_t>(index),
                                                   name),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
        }
        for (std::size_t i = 0; i < importer.file.weak_dependency.size(); ++i) {
            const std::int32_t index = importer.file.weak_dependency[i];
            if (index < 0 ||
                static_cast<std::size_t>(index) >= dependency_count) {
                std::ostringstream message;
                message << "file \"" << importer_name
                        << "\": invalid weak dependency index " << index
                        << " (dependency count " << dependency_count << ")";
                return fail_import(files, importer, unknown_location(),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
            if (!weak_indexes.insert(index).second) {
                // The index is range-checked above, so the dependency and
                // its declaration site are known: keep the position.
                const std::string name =
                    view_text(importer.file.dependency[static_cast<std::size_t>(index)]);
                std::ostringstream message;
                message << "file \"" << importer_name
                        << "\": duplicate weak dependency index " << index;
                return fail_import(files, importer,
                                   import_location(importer,
                                                   static_cast<std::size_t>(index),
                                                   name),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
        }
        for (std::set<std::int32_t>::const_iterator it = public_indexes.begin();
             it != public_indexes.end(); ++it) {
            if (weak_indexes.find(*it) != weak_indexes.end()) {
                const std::string name =
                    view_text(importer.file.dependency[static_cast<std::size_t>(*it)]);
                std::ostringstream message;
                message << "file \"" << importer_name << "\": dependency "
                        << *it << " (\"" << name
                        << "\") is both public and weak";
                return fail_import(files, importer,
                                   import_location(importer,
                                                   static_cast<std::size_t>(*it),
                                                   name),
                                   DIAGNOSTIC_INVALID_IMPORT,
                                   message.str(), error);
            }
        }

        for (std::size_t i = 0; i < dependency_count; ++i) {
            const std::string name = view_text(importer.file.dependency[i]);
            ImportInfo::Modifier modifier = ImportInfo::NORMAL_IMPORT;
            const std::int32_t index = static_cast<std::int32_t>(i);
            if (public_indexes.find(index) != public_indexes.end()) {
                modifier = ImportInfo::PUBLIC_IMPORT;
            } else if (weak_indexes.find(index) != weak_indexes.end()) {
                modifier = ImportInfo::WEAK_IMPORT;
            }
            SchemaFile* target = files.find_file(name);
            if (target == 0) {
                if (allow_missing) {
                    ImportEdge edge;
                    edge.target = 0;
                    edge.dependency_index = i;
                    edge.modifier = modifier;
                    importer.edges.push_back(edge);
                    continue;
                }
                std::ostringstream message;
                message << "file \"" << importer_name << "\": missing "
                        << modifier_text(modifier) << "import \"" << name
                        << "\"";
                return fail_import(files, importer,
                                   import_location(importer, i, name),
                                   DIAGNOSTIC_MISSING_IMPORT,
                                   message.str(), error);
            }
            ImportEdge edge;
            edge.target = target;
            edge.dependency_index = i;
            edge.modifier = modifier;
            importer.edges.push_back(edge);
        }
    }

    // Cycle detection over bound edges. Null targets from allow_missing
    // mode are skipped; cycles among available files are still rejected.
    // The discovery cache prevents infinite reads, but this common pass
    // owns final graph-cycle validation for every frontend.
    std::map<SchemaFile*, int> color;
    for (std::size_t i = 0; i < owned.size(); ++i) color[owned[i].get()] = 0;
    std::vector<SchemaFile*> stack;

    for (std::size_t root = 0; root < owned.size(); ++root) {
        if (color[owned[root].get()] != 0) continue;
        // Iterative depth-first search keeps the active chain explicit so
        // the reported cycle names every edge including the closure.
        std::vector<std::pair<SchemaFile*, std::size_t> > work;
        work.push_back(std::make_pair(owned[root].get(), 0));
        color[owned[root].get()] = 1;
        stack.push_back(owned[root].get());

        while (!work.empty()) {
            SchemaFile* current = work.back().first;
            std::size_t& next = work.back().second;
            // Skip null targets from compatibility mode.
            while (next < current->edges.size() &&
                   current->edges[next].target == 0) {
                ++next;
            }
            if (next >= current->edges.size()) {
                color[current] = 2;
                stack.pop_back();
                work.pop_back();
                continue;
            }
            SchemaFile* target = current->edges[next].target;
            const std::size_t edge_index = current->edges[next].dependency_index;
            ++next;
            const int target_color = color[target];
            if (target_color == 0) {
                color[target] = 1;
                stack.push_back(target);
                work.push_back(std::make_pair(target, 0));
            } else if (target_color == 1) {
                // Report the whole discovery chain from the DFS-tree root
                // through the cycle instead of only the closed loop, so
                // the diagnostic carries the root-to-failure path (for
                // example "root.proto -> middle.proto -> a.proto ->
                // b.proto -> a.proto").
                std::vector<SchemaFile*> chain = stack;
                chain.push_back(target);
                const std::string edge_name =
                    view_text(current->file.dependency[edge_index]);
                std::ostringstream message;
                message << cycle_text(chain) << " (import \"" << edge_name
                        << "\" in \"" << importing_name(*current)
                        << "\" closes the cycle)";
                const SourceLocation where =
                    import_location(*current, edge_index, edge_name);
                // error.file is the file whose declaration closes the
                // cycle; the message itself names the discovery chain
                // from the DFS-tree root with the entry point repeated
                // at the end (for example "a.proto -> a.proto").
                error.file = importing_name(*current);
                error.location = where;
                error.message = message.str();
                error.warning = false;
                error.code = DIAGNOSTIC_IMPORT_CYCLE;
                clear_all_edges(files);
                return false;
            }
        }
    }

    return true;
}

} // namespace easypb_schema
