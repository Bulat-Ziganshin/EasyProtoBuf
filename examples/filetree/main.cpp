#include "filetree.pb.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Total terminal width of the single-line scan progress indicator.
constexpr std::size_t progress_line_width = 99;

namespace filetree_benchmark
{

constexpr std::size_t kib = 1024;
constexpr std::size_t mib = 1024 * kib;
constexpr std::size_t first_arena_buffer = 4 * kib;
constexpr std::size_t maximum_arena_buffer = 2 * mib;

template <typename Operation>
bool try_scan_operation(std::uint64_t& errors, Operation&& operation)
{
    try
    {
        std::forward<Operation>(operation)();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        ++errors;
        return false;
    }
}

std::string path_text(const fs::path& path)
{
    return path.generic_u8string();
}

class NameArena
{
public:
    std::string_view copy(std::string_view value)
    {
        if (value.empty())
            return {};

        if (blocks_.empty() || blocks_.back().capacity - blocks_.back().used < value.size())
            add_block(value.size());

        Block& block = blocks_.back();
        char* destination = block.data.get() + block.used;
        std::memcpy(destination, value.data(), value.size());
        block.used += value.size();
        used_bytes_ += value.size();
        return {destination, value.size()};
    }

    std::size_t used_bytes() const noexcept
    {
        return used_bytes_;
    }

    std::size_t allocated_bytes() const noexcept
    {
        return allocated_bytes_;
    }

    std::size_t block_count() const noexcept
    {
        return blocks_.size();
    }

private:
    struct Block
    {
        std::unique_ptr<char[]> data;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    void add_block(std::size_t minimum_capacity)
    {
        std::size_t capacity = next_capacity_;

        if (minimum_capacity > maximum_arena_buffer)
        {
            capacity = minimum_capacity;
        }
        else
        {
            while (capacity < minimum_capacity && capacity < maximum_arena_buffer)
                capacity *= 2;
            capacity = std::min(capacity, maximum_arena_buffer);
        }

        Block block;
        block.data = std::make_unique<char[]>(capacity);
        block.capacity = capacity;
        blocks_.push_back(std::move(block));
        allocated_bytes_ += capacity;

        if (capacity <= maximum_arena_buffer)
            next_capacity_ = std::min(capacity * 2, maximum_arena_buffer);
    }

    std::vector<Block> blocks_;
    std::size_t next_capacity_ = first_arena_buffer;
    std::size_t used_bytes_ = 0;
    std::size_t allocated_bytes_ = 0;
};

struct Utf8Prefix
{
    std::string text;
    std::size_t characters = 0;
};

bool is_utf8_continuation(unsigned char byte)
{
    return (byte & 0xc0U) == 0x80U;
}

Utf8Prefix first_utf8_characters(const std::string& text, std::size_t limit)
{
    std::size_t offset = 0;
    std::size_t characters = 0;

    while (offset < text.size() && characters < limit)
    {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t length = 1;
        if ((lead & 0xe0U) == 0xc0U)
            length = 2;
        else if ((lead & 0xf0U) == 0xe0U)
            length = 3;
        else if ((lead & 0xf8U) == 0xf0U)
            length = 4;

        if (offset + length > text.size())
            length = 1;
        else
        {
            for (std::size_t i = 1; i < length; ++i)
            {
                if (!is_utf8_continuation(static_cast<unsigned char>(text[offset + i])))
                {
                    length = 1;
                    break;
                }
            }
        }

        offset += length;
        ++characters;
    }

    return {text.substr(0, offset), characters};
}

std::string format_grouped_integer(std::uint64_t value)
{
    std::string text = std::to_string(value);
    for (std::size_t position = text.size(); position > 3; position -= 3)
        text.insert(position - 3, 1, '\'');
    return text;
}

std::string format_fixed(double value, int precision)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::string format_grouped_fixed(double value, int precision)
{
    std::string text = format_fixed(value, precision);
    const std::size_t first_digit = !text.empty() && (text.front() == '-' || text.front() == '+')
        ? 1
        : 0;
    const std::size_t decimal = text.find('.');
    const std::size_t integer_end = decimal == std::string::npos ? text.size() : decimal;

    for (std::size_t position = integer_end; position > first_digit + 3; position -= 3)
        text.insert(position - 3, 1, '\'');
    return text;
}

bool should_check_progress(
    std::uint64_t files,
    std::uint64_t directories) noexcept
{
    return ((files + directories) & 255U) == 0;
}

class ProgressReporter
{
public:
    using clock = std::chrono::steady_clock;

    explicit ProgressReporter(clock::time_point start)
        : start_(start), next_render_(start + std::chrono::seconds(1))
    {
    }

    ProgressReporter(const ProgressReporter&) = delete;
    ProgressReporter& operator=(const ProgressReporter&) = delete;

    ~ProgressReporter()
    {
        clear();
    }

    void set_directory(std::string relative_directory)
    {
        current_directory_ = std::move(relative_directory);
    }

    void file_found()
    {
        ++files_;
        entry_found();
    }

    void directory_found()
    {
        ++directories_;
        entry_found();
    }

    void finish()
    {
        if (finished_)
            return;

        render(clock::now());
        finished_ = true;
    }

    void clear()
    {
        if (!line_active_)
            return;

        std::string output;
        output.reserve(previous_line_size_ + 2);
        output.push_back('\r');
        output.append(previous_line_size_, ' ');
        output.push_back('\r');
        std::cerr.write(output.data(), static_cast<std::streamsize>(output.size()));
        std::cerr.flush();

        previous_line_size_ = 0;
        line_active_ = false;
        finished_ = true;
    }

private:
    void entry_found()
    {
        if (should_check_progress(files_, directories_))
            render_if_due();
    }

    void render_if_due()
    {
        const clock::time_point now = clock::now();
        if (now < next_render_)
            return;

        render(now);
        const auto whole_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - start_);
        next_render_ = start_ + whole_seconds + std::chrono::seconds(1);
    }

    void render(clock::time_point now)
    {
        const auto elapsed_duration = now - start_;
        const std::uint64_t elapsed_seconds = static_cast<std::uint64_t>(
            std::max<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed_duration).count(),
                0));
        const double rate_seconds = std::max(
            std::chrono::duration<double>(elapsed_duration).count(),
            1e-12);
        const std::uint64_t entries = files_ + directories_;

        std::ostringstream suffix_stream;
        suffix_stream.imbue(std::locale::classic());
        suffix_stream << "  files " << format_grouped_integer(files_)
                      << "  dirs " << format_grouped_integer(directories_)
                      << "  " << format_grouped_fixed(
                             static_cast<double>(entries) / rate_seconds,
                             0)
                      << " entries/s"
                      << "  " << format_grouped_integer(elapsed_seconds) << " s";
        const std::string suffix = suffix_stream.str();

        std::string line;
        if (suffix.size() >= progress_line_width)
        {
            line = suffix.substr(suffix.size() - progress_line_width);
        }
        else
        {
            const std::size_t directory_width = progress_line_width - suffix.size();
            const Utf8Prefix prefix =
                first_utf8_characters(current_directory_, directory_width);
            line = prefix.text;
            if (prefix.characters < directory_width)
                line.append(directory_width - prefix.characters, ' ');
            line += suffix;
        }

        std::string output;
        output.reserve(1 + std::max(line.size(), previous_line_size_));
        output.push_back('\r');
        output += line;
        if (line.size() < previous_line_size_)
            output.append(previous_line_size_ - line.size(), ' ');

        std::cerr.write(output.data(), static_cast<std::streamsize>(output.size()));
        std::cerr.flush();
        previous_line_size_ = line.size();
        line_active_ = true;
    }

    const clock::time_point start_;
    clock::time_point next_render_;
    std::uint64_t files_ = 0;
    std::uint64_t directories_ = 0;
    std::string current_directory_ = ".";
    std::size_t previous_line_size_ = 0;
    bool line_active_ = false;
    bool finished_ = false;
};

class FileTimeConverter
{
public:
    FileTimeConverter()
        : file_anchor_(fs::file_time_type::clock::now()),
          system_anchor_(std::chrono::system_clock::now())
    {
    }

    std::int64_t to_unix_nanoseconds(fs::file_time_type value) const noexcept
    {
        using floating_nanoseconds = std::chrono::duration<long double, std::nano>;
        const long double system_anchor_ns =
            floating_nanoseconds(system_anchor_.time_since_epoch()).count();
        const long double delta_ns = floating_nanoseconds(value - file_anchor_).count();
        const long double result = system_anchor_ns + delta_ns;
        const long double minimum =
            static_cast<long double>(std::numeric_limits<std::int64_t>::min());
        const long double maximum =
            static_cast<long double>(std::numeric_limits<std::int64_t>::max());

        if (result <= minimum)
            return std::numeric_limits<std::int64_t>::min();
        if (result >= maximum)
            return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(result);
    }

private:
    fs::file_time_type file_anchor_;
    std::chrono::system_clock::time_point system_anchor_;
};

struct ScanContext
{
    NameArena& arena;
    ProgressReporter& progress;
    const fs::path& root;
    FileTimeConverter time_converter;
    std::uint64_t errors = 0;
};

std::string root_name(const fs::path& root)
{
    std::string name = path_text(root.filename());
    if (name.empty())
        name = path_text(root.root_name());
    if (name.empty())
        name = path_text(root.root_path());
    if (name.empty())
        name = path_text(root);
    if (name.empty())
        name = ".";
    return name;
}

std::string entry_name(const fs::path& path)
{
    std::string name = path_text(path.filename());
    return name.empty() ? path_text(path) : name;
}

void set_current_directory(const fs::path& directory, ScanContext& context)
{
    fs::path relative = directory.lexically_relative(context.root);
    std::string text = path_text(relative);
    if (text.empty())
        text = ".";
    context.progress.set_directory(std::move(text));
}

void set_permissions(
    filetree::Node& node,
    const fs::file_status& status)
{
    if (status.permissions() == fs::perms::unknown)
        return;

    node.permissions = static_cast<std::uint32_t>(status.permissions());
    node.has_permissions = true;
}

void set_last_write_time(
    filetree::Node& node,
    const fs::path& path,
    ScanContext& context)
{
    std::error_code error;
    const fs::file_time_type time = fs::last_write_time(path, error);
    if (error)
    {
        ++context.errors;
        return;
    }

    node.last_write_time_unix_ns =
        context.time_converter.to_unix_nanoseconds(time);
    node.has_last_write_time_unix_ns = true;
}

std::uint32_t node_kind(const fs::file_status& status)
{
    if (fs::is_symlink(status))
        return filetree::symbolic_link;
    if (fs::is_regular_file(status))
        return filetree::regular_file;
    if (fs::is_directory(status))
        return filetree::directory;
    return filetree::other;
}

filetree::Node make_entry_node(
    const fs::directory_entry& entry,
    ScanContext& context)
{
    filetree::Node node;
    const std::string name = entry_name(entry.path());
    node.name = context.arena.copy(name);
    node.has_name = true;

    std::error_code status_error;
    const fs::file_status status = entry.symlink_status(status_error);
    if (status_error)
        ++context.errors;

    node.kind = status_error ? filetree::other : node_kind(status);
    node.has_kind = true;

    if (!status_error)
        set_permissions(node, status);
    set_last_write_time(node, entry.path(), context);

    if (node.kind == filetree::regular_file)
    {
        std::error_code size_error;
        const std::uintmax_t size = entry.file_size(size_error);
        if (size_error)
            ++context.errors;
        else
        {
            node.size = size > std::numeric_limits<std::uint64_t>::max()
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(size);
            node.has_size = true;
        }
        context.progress.file_found();
    }
    else if (node.kind == filetree::directory)
    {
        context.progress.directory_found();
    }
    else if (node.kind == filetree::symbolic_link)
    {
        std::error_code target_error;
        const fs::path target = fs::read_symlink(entry.path(), target_error);
        if (target_error)
            ++context.errors;
        else
        {
            (void)try_scan_operation(context.errors, [&]
            {
                const std::string target_text = path_text(target);
                node.symlink_target = context.arena.copy(target_text);
                node.has_symlink_target = true;
            });
        }
    }

    return node;
}

void scan_directory(
    const fs::path& directory,
    filetree::Node& directory_node,
    ScanContext& context)
{
    (void)try_scan_operation(context.errors, [&]
    {
        set_current_directory(directory, context);
    });

    std::error_code iterator_error;
    std::optional<fs::directory_iterator> iterator;
    if (!try_scan_operation(context.errors, [&]
        {
            iterator.emplace(directory, fs::directory_options::none, iterator_error);
        }))
    {
        return;
    }
    if (iterator_error)
    {
        ++context.errors;
        return;
    }

    const fs::directory_iterator end;
    while (*iterator != end)
    {
        std::optional<fs::directory_entry> entry;
        if (try_scan_operation(context.errors, [&] { entry.emplace(**iterator); }))
        {
            std::optional<filetree::Node> child;
            if (try_scan_operation(context.errors, [&]
                {
                    child.emplace(make_entry_node(*entry, context));
                }))
            {
                const bool recurse = child->kind == filetree::directory;
                directory_node.children.push_back(std::move(*child));

                if (recurse)
                {
                    scan_directory(
                        entry->path(),
                        directory_node.children.back(),
                        context);
                    (void)try_scan_operation(context.errors, [&]
                    {
                        set_current_directory(directory, context);
                    });
                }
            }
        }

        iterator_error.clear();
        if (!try_scan_operation(context.errors, [&]
            {
                iterator->increment(iterator_error);
            }))
        {
            break;
        }
        if (iterator_error)
        {
            ++context.errors;
            break;
        }
    }
}

filetree::FileTree scan_tree(
    const fs::path& root,
    NameArena& arena,
    ProgressReporter& progress,
    std::uint64_t& errors)
{
    ScanContext context{arena, progress, root, FileTimeConverter{}, 0};

    filetree::FileTree tree;
    tree.has_root = true;

    std::string name = ".";
    (void)try_scan_operation(context.errors, [&] { name = root_name(root); });
    tree.root.name = arena.copy(name);
    tree.root.has_name = true;
    tree.root.kind = filetree::directory;
    tree.root.has_kind = true;

    std::error_code status_error;
    const fs::file_status status = fs::status(root, status_error);
    if (status_error)
        ++context.errors;
    else
        set_permissions(tree.root, status);
    set_last_write_time(tree.root, root, context);

    progress.directory_found();
    scan_directory(root, tree.root, context);
    errors = context.errors;
    return tree;
}

struct NameStatistics
{
    std::uint64_t count = 0;
    std::uint64_t total_bytes = 0;

    double average_bytes() const noexcept
    {
        return count == 0
            ? 0.0
            : static_cast<double>(total_bytes) / static_cast<double>(count);
    }

    bool operator==(const NameStatistics& other) const noexcept
    {
        return count == other.count && total_bytes == other.total_bytes;
    }
};

struct TreeStatistics
{
    std::uint64_t regular_files = 0;
    std::uint64_t directories = 0;
    std::uint64_t symbolic_links = 0;
    std::uint64_t other_nodes = 0;
    std::uint64_t entries = 0;
    std::uint64_t logical_file_bytes = 0;
    NameStatistics file_names;
    NameStatistics directory_names;
    NameStatistics other_names;

    bool operator==(const TreeStatistics& other) const noexcept
    {
        return regular_files == other.regular_files &&
               directories == other.directories &&
               symbolic_links == other.symbolic_links &&
               other_nodes == other.other_nodes &&
               entries == other.entries &&
               logical_file_bytes == other.logical_file_bytes &&
               file_names == other.file_names &&
               directory_names == other.directory_names &&
               other_names == other.other_names;
    }
};

void collect_statistics(const filetree::Node& node, TreeStatistics& statistics)
{
    ++statistics.entries;

    switch (node.kind)
    {
        case filetree::regular_file:
            ++statistics.regular_files;
            ++statistics.file_names.count;
            statistics.file_names.total_bytes += node.name.size();
            if (node.has_size)
                statistics.logical_file_bytes += node.size;
            break;
        case filetree::directory:
            ++statistics.directories;
            ++statistics.directory_names.count;
            statistics.directory_names.total_bytes += node.name.size();
            break;
        case filetree::symbolic_link:
            ++statistics.symbolic_links;
            ++statistics.other_names.count;
            statistics.other_names.total_bytes += node.name.size();
            break;
        default:
            ++statistics.other_nodes;
            ++statistics.other_names.count;
            statistics.other_names.total_bytes += node.name.size();
            break;
    }

    for (const filetree::Node& child : node.children)
        collect_statistics(child, statistics);
}

TreeStatistics collect_statistics(const filetree::FileTree& tree)
{
    TreeStatistics statistics;
    collect_statistics(tree.root, statistics);
    return statistics;
}

bool equivalent(const filetree::Node& left, const filetree::Node& right)
{
    if (left.name != right.name ||
        left.kind != right.kind ||
        left.has_size != right.has_size ||
        left.has_last_write_time_unix_ns != right.has_last_write_time_unix_ns ||
        left.has_permissions != right.has_permissions ||
        left.has_symlink_target != right.has_symlink_target ||
        left.children.size() != right.children.size())
        return false;

    if (left.has_size && left.size != right.size)
        return false;
    if (left.has_last_write_time_unix_ns &&
        left.last_write_time_unix_ns != right.last_write_time_unix_ns)
        return false;
    if (left.has_permissions && left.permissions != right.permissions)
        return false;
    if (left.has_symlink_target && left.symlink_target != right.symlink_target)
        return false;

    for (std::size_t i = 0; i < left.children.size(); ++i)
    {
        if (!equivalent(left.children[i], right.children[i]))
            return false;
    }
    return true;
}

bool equivalent(const filetree::FileTree& left, const filetree::FileTree& right)
{
    return left.has_root == right.has_root &&
           (!left.has_root || equivalent(left.root, right.root));
}

struct StageResult
{
    const char* name;
    double seconds;
    double mebibytes_per_second;
    double entries_per_second;
};

double rate(std::uint64_t amount, double seconds)
{
    return static_cast<double>(amount) / std::max(seconds, 1e-12);
}

void print_name_statistics(const char* label, const NameStatistics& statistics)
{
    const std::string count = format_grouped_integer(statistics.count);
    const std::string total_bytes = format_grouped_integer(statistics.total_bytes);
    const std::string average_bytes = format_fixed(statistics.average_bytes(), 2);

    std::cout << std::left << std::setw(18) << label
              << std::right << std::setw(14) << count
              << std::setw(18) << total_bytes
              << std::setw(18) << average_bytes << '\n';
}

void print_stage(const StageResult& stage)
{
    const std::string seconds = format_fixed(stage.seconds, 6);
    const std::string mebibytes_per_second =
        format_fixed(stage.mebibytes_per_second, 2);
    const std::string entries_per_second =
        format_grouped_fixed(stage.entries_per_second, 0);

    std::cout << std::left << std::setw(10) << stage.name
              << std::right << std::setw(13) << seconds
              << std::setw(15) << mebibytes_per_second
              << std::setw(17) << entries_per_second << '\n';
}

void print_report(
    const fs::path& root,
    const TreeStatistics& statistics,
    std::uint64_t scan_errors,
    const NameArena& arena,
    std::size_t wire_size,
    double scan_seconds,
    double encode_seconds,
    double decode_seconds)
{
    const double logical_mib = static_cast<double>(statistics.logical_file_bytes) / mib;
    const double wire_mib = static_cast<double>(wire_size) / mib;

    std::string root_text = "<unprintable path>";
    std::uint64_t ignored_display_errors = 0;
    (void)try_scan_operation(ignored_display_errors, [&] { root_text = path_text(root); });

    std::cout << "Scanned " << root_text
              << ", found " << format_grouped_integer(statistics.logical_file_bytes)
              << " bytes (" << format_grouped_fixed(logical_mib, 3)
              << " MiB), scan errors: " << format_grouped_integer(scan_errors) << "\n\n";

    NameStatistics total_names;
    total_names.count = statistics.file_names.count +
                        statistics.directory_names.count +
                        statistics.other_names.count;
    total_names.total_bytes = statistics.file_names.total_bytes +
                              statistics.directory_names.total_bytes +
                              statistics.other_names.total_bytes;

    std::cout << std::left << std::setw(18) << ""
              << std::right << std::setw(14) << "Count"
              << std::setw(18) << "Total bytes"
              << std::setw(18) << "Average bytes" << '\n';
    print_name_statistics("File names", statistics.file_names);
    print_name_statistics("Directory names", statistics.directory_names);
    print_name_statistics("Other nodes", statistics.other_names);
    print_name_statistics("TOTAL", total_names);

    std::cout << '\n'
              << "Name arena:  used "
              << format_grouped_integer(static_cast<std::uint64_t>(arena.used_bytes()))
              << " bytes,  allocated "
              << format_grouped_integer(static_cast<std::uint64_t>(arena.allocated_bytes()))
              << " bytes = "
              << format_grouped_integer(static_cast<std::uint64_t>(arena.block_count()))
              << " buffers\n"
              << "Serialized buffer:  "
              << format_grouped_integer(static_cast<std::uint64_t>(wire_size))
              << " bytes (" << format_grouped_fixed(wire_mib, 3) << " MiB)\n\n"
              << std::left << std::setw(10) << "Stage"
              << std::right << std::setw(13) << "Time (s)"
              << std::setw(15) << "MiB/s"
              << std::setw(17) << "Entries/s" << '\n';

    print_stage({
        "Scan",
        scan_seconds,
        wire_mib / std::max(scan_seconds, 1e-12),
        rate(statistics.entries, scan_seconds)});
    print_stage({
        "Encode",
        encode_seconds,
        wire_mib / std::max(encode_seconds, 1e-12),
        rate(statistics.entries, encode_seconds)});
    print_stage({
        "Decode",
        decode_seconds,
        wire_mib / std::max(decode_seconds, 1e-12),
        rate(statistics.entries, decode_seconds)});
}

} // namespace filetree_benchmark

int main(int argc, char** argv)
{
    using clock = std::chrono::steady_clock;
    using namespace filetree_benchmark;

    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <directory>\n";
        return 1;
    }

    try
    {
        std::error_code path_error;
        fs::path root = fs::absolute(fs::path(argv[1]), path_error).lexically_normal();
        if (path_error)
            throw std::runtime_error("cannot make the scan path absolute: " + path_error.message());

        std::error_code status_error;
        const fs::file_status root_status = fs::status(root, status_error);
        if (status_error)
            throw std::runtime_error("cannot access scan root: " + status_error.message());
        if (!fs::is_directory(root_status))
            throw std::runtime_error("scan root is not a directory");

        NameArena arena;
        std::uint64_t scan_errors = 0;

        const clock::time_point scan_start = clock::now();
        ProgressReporter progress(scan_start);
        filetree::FileTree source = scan_tree(root, arena, progress, scan_errors);
        const clock::time_point scan_end = clock::now();
        progress.finish();

        const TreeStatistics source_statistics = collect_statistics(source);

        const clock::time_point encode_start = clock::now();
        std::string wire = easypb::encode(source);
        const clock::time_point encode_end = clock::now();

        const clock::time_point decode_start = clock::now();
        filetree::FileTree decoded = easypb::decode<filetree::FileTree>(wire);
        const clock::time_point decode_end = clock::now();

        const TreeStatistics decoded_statistics = collect_statistics(decoded);
        if (!equivalent(source, decoded))
            throw std::runtime_error("decoded tree differs from the scanned tree");
        if (!(source_statistics == decoded_statistics))
            throw std::runtime_error("decoded aggregate statistics differ from the source");

        const double scan_seconds =
            std::chrono::duration<double>(scan_end - scan_start).count();
        const double encode_seconds =
            std::chrono::duration<double>(encode_end - encode_start).count();
        const double decode_seconds =
            std::chrono::duration<double>(decode_end - decode_start).count();

        progress.clear();
        print_report(
            root,
            source_statistics,
            scan_errors,
            arena,
            wire.size(),
            scan_seconds,
            encode_seconds,
            decode_seconds);
        std::cout << "\nValidation: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }
}
