// Extended deterministic property tests for the common import-graph binder.
// Built only with EASYPB_EXTENDED_TESTS=ON.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "schema.hpp"

namespace {

class Rng
{
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next()
    {
        // xorshift64*: deterministic on every platform.
        std::uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * UINT64_C(2685821657736338717);
    }

    std::size_t bounded(std::size_t limit)
    {
        return limit == 0 ? 0 : static_cast<std::size_t>(next() % limit);
    }

private:
    std::uint64_t state_;
};

struct ExpectedEdge
{
    std::size_t target;
    easypb_schema::ImportInfo::Modifier modifier;
};

std::string file_name(std::size_t index)
{
    std::ostringstream out;
    out << "r" << index << ".proto";
    return out.str();
}

bool reference_has_cycle(const std::vector<std::vector<ExpectedEdge> >& graph)
{
    const std::size_t n = graph.size();
    std::vector<std::size_t> indegree(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < graph[i].size(); ++j) {
            ++indegree[graph[i][j].target];
        }
    }

    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) queue.push_back(i);
    }

    std::size_t visited = 0;
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t current = queue[head];
        ++visited;
        for (std::size_t j = 0; j < graph[current].size(); ++j) {
            const std::size_t target = graph[current][j].target;
            if (--indegree[target] == 0) queue.push_back(target);
        }
    }
    return visited != n;
}

void populate_schema(easypb_schema::SchemaSet& files,
                     const std::vector<std::vector<ExpectedEdge> >& graph)
{
    const std::size_t n = graph.size();
    for (std::size_t i = 0; i < n; ++i) files.add_file(file_name(i));

    for (std::size_t i = 0; i < n; ++i) {
        easypb_schema::SchemaFile* file = files.find_file(file_name(i));
        for (std::size_t j = 0; j < graph[i].size(); ++j) {
            const ExpectedEdge& expected = graph[i][j];
            file->file.dependency.push_back(
                file->strings.save(file_name(expected.target)));
            const std::int32_t index = static_cast<std::int32_t>(j);
            if (expected.modifier == easypb_schema::ImportInfo::PUBLIC_IMPORT) {
                file->file.public_dependency.push_back(index);
            } else if (expected.modifier == easypb_schema::ImportInfo::WEAK_IMPORT) {
                file->file.weak_dependency.push_back(index);
            }
        }
    }
}

bool verify_edges(easypb_schema::SchemaSet& files,
                  const std::vector<std::vector<ExpectedEdge> >& graph,
                  std::string& reason)
{
    for (std::size_t i = 0; i < graph.size(); ++i) {
        easypb_schema::SchemaFile* file = files.find_file(file_name(i));
        if (file == 0 || file->edges.size() != graph[i].size()) {
            reason = "edge count mismatch";
            return false;
        }
        for (std::size_t j = 0; j < graph[i].size(); ++j) {
            const easypb_schema::ImportEdge& actual = file->edges[j];
            const ExpectedEdge& expected = graph[i][j];
            if (actual.target != files.find_file(file_name(expected.target)) ||
                actual.dependency_index != j ||
                actual.modifier != expected.modifier) {
                reason = "edge target/order/modifier mismatch";
                return false;
            }
        }
    }
    return true;
}

std::vector<std::vector<ExpectedEdge> > make_random_graph(Rng& rng,
                                                          std::size_t n,
                                                          bool force_dag)
{
    std::vector<std::vector<ExpectedEdge> > graph(n);
    const std::size_t density = 1 + rng.bounded(force_dag ? 35 : 12);

    for (std::size_t from = 0; from < n; ++from) {
        for (std::size_t to = 0; to < n; ++to) {
            if (force_dag && to <= from) continue;
            if (rng.bounded(100) >= density) continue;

            ExpectedEdge edge;
            edge.target = to;
            const std::size_t modifier = rng.bounded(20);
            edge.modifier = modifier == 0
                ? easypb_schema::ImportInfo::PUBLIC_IMPORT
                : modifier == 1
                    ? easypb_schema::ImportInfo::WEAK_IMPORT
                    : easypb_schema::ImportInfo::NORMAL_IMPORT;
            graph[from].push_back(edge);
        }
    }
    return graph;
}

bool run_existing_graph_case(const std::vector<std::vector<ExpectedEdge> >& graph,
                             std::size_t case_index)
{
    easypb_schema::SchemaSet files;
    populate_schema(files, graph);

    const bool expected_cycle = reference_has_cycle(graph);
    easypb_schema::Diagnostic error;
    const bool ok = easypb_schema::bind_import_edges(files, false, error);
    if (ok == expected_cycle) {
        std::cerr << "random graph case " << case_index
                  << " disagrees with reference cycle result\n";
        return false;
    }
    if (!ok) {
        if (error.code != easypb_schema::DIAGNOSTIC_IMPORT_CYCLE) {
            std::cerr << "random graph case " << case_index
                      << " failed with non-cycle diagnostic\n";
            return false;
        }
        const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& owned =
            files.files();
        for (std::size_t i = 0; i < owned.size(); ++i) {
            if (!owned[i]->edges.empty()) {
                std::cerr << "random graph case " << case_index
                          << " left edges after cycle failure\n";
                return false;
            }
        }
        return true;
    }

    std::string reason;
    if (!verify_edges(files, graph, reason)) {
        std::cerr << "random graph case " << case_index << ": " << reason << '\n';
        return false;
    }
    return true;
}

bool run_missing_edge_cases(Rng& rng)
{
    for (std::size_t case_index = 0; case_index < 2000; ++case_index) {
        const std::size_t n = 5 + rng.bounded(46);
        easypb_schema::SchemaSet files;
        for (std::size_t i = 0; i < n; ++i) files.add_file(file_name(i));

        std::size_t null_edges_expected = 0;
        for (std::size_t i = 0; i < n; ++i) {
            easypb_schema::SchemaFile* file = files.find_file(file_name(i));
            const std::size_t deps = 1 + rng.bounded(8);
            for (std::size_t j = 0; j < deps; ++j) {
                std::string dependency;
                bool is_missing = false;
                if (rng.bounded(4) == 0) {
                    std::ostringstream missing;
                    missing << "missing-" << case_index << '-' << i << '-' << j
                            << ".proto";
                    dependency = missing.str();
                    is_missing = true;
                } else {
                    // Only forward edges among existing files, so the
                    // available portion is guaranteed acyclic.
                    if (i + 1 >= n) {
                        std::ostringstream missing;
                        missing << "missing-tail-" << case_index << '-' << j
                                << ".proto";
                        dependency = missing.str();
                        is_missing = true;
                    } else {
                        dependency = file_name(i + 1 + rng.bounded(n - i - 1));
                    }
                }
                // Avoid duplicate dependency spellings in one descriptor.
                bool duplicate = false;
                for (std::size_t k = 0; k < file->file.dependency.size(); ++k) {
                    const str_view& existing = file->file.dependency[k];
                    if (std::string(existing.data(), existing.size()) == dependency) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                file->file.dependency.push_back(file->strings.save(dependency));
                if (is_missing) ++null_edges_expected;
            }
        }

        easypb_schema::Diagnostic error;
        if (!easypb_schema::bind_import_edges(files, true, error)) {
            std::cerr << "allow_missing case " << case_index
                      << " unexpectedly failed: " << error.message << '\n';
            return false;
        }

        std::size_t null_edges_actual = 0;
        const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& owned =
            files.files();
        for (std::size_t i = 0; i < owned.size(); ++i) {
            for (std::size_t j = 0; j < owned[i]->edges.size(); ++j) {
                if (owned[i]->edges[j].target == 0) ++null_edges_actual;
            }
        }
        // Zero missing dependencies in a case is a valid RNG outcome,
        // not a binder failure: only a count mismatch proves anything.
        if (null_edges_actual != null_edges_expected) {
            std::cerr << "allow_missing case " << case_index
                      << " produced an invalid null-edge count\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    Rng rng(UINT64_C(0x5eeda11ce5eed123));
    const std::size_t random_cases = 10000;

    for (std::size_t i = 0; i < random_cases; ++i) {
        const std::size_t n = 5 + rng.bounded(96);
        const bool force_dag = (i % 2) == 0;
        const std::vector<std::vector<ExpectedEdge> > graph =
            make_random_graph(rng, n, force_dag);
        if (!run_existing_graph_case(graph, i)) return EXIT_FAILURE;
    }

    if (!run_missing_edge_cases(rng)) return EXIT_FAILURE;

    std::cout << "extended import graph properties passed: "
              << random_cases << " random graphs plus 2000 allow-missing cases\n";
    return EXIT_SUCCESS;
}
