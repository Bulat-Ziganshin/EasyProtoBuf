// Exhaustive property test for the common import-graph binder.
//
// Enumerates every directed graph with 1..4 vertices, including self edges:
//   2^(1^2) + 2^(2^2) + 2^(3^2) + 2^(4^2) = 66,066 graphs.
//
// The reference result is computed independently with transitive closure.
// This test is intentionally part of the default suite: it is deterministic,
// in-memory, and cheap enough to run on every supported compiler.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "schema.hpp"

namespace {

std::size_t graphs_checked = 0;
int failures = 0;

std::string file_name(std::size_t index)
{
    std::ostringstream out;
    out << "g" << index << ".proto";
    return out.str();
}

easypb_schema::SchemaFile& add_file(easypb_schema::SchemaSet& set,
                                    std::size_t index,
                                    std::size_t vertex_count,
                                    std::uint64_t mask)
{
    easypb_schema::SchemaFile& file = set.add_file(file_name(index));
    for (std::size_t target = 0; target < vertex_count; ++target) {
        const std::size_t bit = index * vertex_count + target;
        if ((mask & (std::uint64_t(1) << bit)) != 0) {
            file.file.dependency.push_back(file.strings.save(file_name(target)));
        }
    }
    return file;
}

bool reference_has_cycle(std::size_t vertex_count, std::uint64_t mask)
{
    bool reach[4][4] = {{false, false, false, false},
                        {false, false, false, false},
                        {false, false, false, false},
                        {false, false, false, false}};

    for (std::size_t from = 0; from < vertex_count; ++from) {
        for (std::size_t to = 0; to < vertex_count; ++to) {
            const std::size_t bit = from * vertex_count + to;
            reach[from][to] = (mask & (std::uint64_t(1) << bit)) != 0;
        }
    }

    for (std::size_t via = 0; via < vertex_count; ++via) {
        for (std::size_t from = 0; from < vertex_count; ++from) {
            for (std::size_t to = 0; to < vertex_count; ++to) {
                reach[from][to] = reach[from][to] ||
                                  (reach[from][via] && reach[via][to]);
            }
        }
    }

    for (std::size_t i = 0; i < vertex_count; ++i) {
        if (reach[i][i]) return true;
    }
    return false;
}

void report_failure(std::size_t vertex_count, std::uint64_t mask,
                    const std::string& message)
{
    if (failures < 20) {
        std::cerr << "N=" << vertex_count << " mask=" << mask
                  << ": " << message << '\n';
    }
    ++failures;
}

void check_acyclic_edges(easypb_schema::SchemaSet& files,
                         std::size_t vertex_count, std::uint64_t mask)
{
    for (std::size_t from = 0; from < vertex_count; ++from) {
        easypb_schema::SchemaFile* file = files.find_file(file_name(from));
        if (file == 0) {
            report_failure(vertex_count, mask, "binder lost a schema file");
            return;
        }

        std::size_t expected_index = 0;
        for (std::size_t to = 0; to < vertex_count; ++to) {
            const std::size_t bit = from * vertex_count + to;
            if ((mask & (std::uint64_t(1) << bit)) == 0) continue;

            if (expected_index >= file->edges.size()) {
                report_failure(vertex_count, mask,
                               "binder produced too few import edges");
                return;
            }
            const easypb_schema::ImportEdge& edge = file->edges[expected_index];
            if (edge.target != files.find_file(file_name(to)) ||
                edge.dependency_index != expected_index ||
                edge.modifier != easypb_schema::ImportInfo::NORMAL_IMPORT) {
                report_failure(vertex_count, mask,
                               "bound edge differs from descriptor order");
                return;
            }
            ++expected_index;
        }
        if (file->edges.size() != expected_index) {
            report_failure(vertex_count, mask,
                           "binder produced too many import edges");
            return;
        }
    }
}

void check_cycle_cleanup(easypb_schema::SchemaSet& files,
                         std::size_t vertex_count, std::uint64_t mask)
{
    const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& owned =
        files.files();
    for (std::size_t i = 0; i < owned.size(); ++i) {
        if (!owned[i]->edges.empty()) {
            report_failure(vertex_count, mask,
                           "cycle failure left partially bound edges");
            return;
        }
    }
}

void check_graph(std::size_t vertex_count, std::uint64_t mask)
{
    easypb_schema::SchemaSet files;
    for (std::size_t i = 0; i < vertex_count; ++i) {
        add_file(files, i, vertex_count, mask);
    }

    const bool expected_cycle = reference_has_cycle(vertex_count, mask);
    easypb_schema::Diagnostic error;
    const bool ok = easypb_schema::bind_import_edges(files, false, error);

    if (ok == expected_cycle) {
        report_failure(vertex_count, mask,
                       expected_cycle ? "reference found a cycle but binder succeeded"
                                      : "reference found no cycle but binder failed");
    } else if (ok) {
        check_acyclic_edges(files, vertex_count, mask);
    } else {
        if (error.code != easypb_schema::DIAGNOSTIC_IMPORT_CYCLE) {
            report_failure(vertex_count, mask,
                           "cyclic graph failed with the wrong diagnostic code");
        }
        check_cycle_cleanup(files, vertex_count, mask);
    }

    ++graphs_checked;
}

} // namespace

int main()
{
    for (std::size_t vertex_count = 1; vertex_count <= 4; ++vertex_count) {
        const std::size_t bits = vertex_count * vertex_count;
        const std::uint64_t graph_count = std::uint64_t(1) << bits;
        for (std::uint64_t mask = 0; mask < graph_count; ++mask) {
            check_graph(vertex_count, mask);
        }
    }

    if (graphs_checked != 66066u) {
        std::cerr << "internal test error: expected 66066 graphs, checked "
                  << graphs_checked << '\n';
        return EXIT_FAILURE;
    }
    if (failures != 0) {
        std::cerr << failures << " exhaustive graph check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "checked all " << graphs_checked
              << " directed import graphs with N <= 4\n";
    return EXIT_SUCCESS;
}
