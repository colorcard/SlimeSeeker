#include "worldgen26/worldgen26.hpp"
#include "worldgen26/cubiomes_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace ss::worldgen26 {
namespace {

struct Parameter {
    int64_t min;
    int64_t max;
};

struct ParameterPoint {
    std::array<Parameter, 7> space;
    BiomeProfile profile;
};

constexpr ParameterPoint kParameterPoints[] = {
#include "worldgen26/generated/biome_parameters_26_2.inc"
};

struct Node {
    std::array<Parameter, 7> space{};
    std::vector<std::shared_ptr<Node>> children;
    const ParameterPoint *leaf = nullptr;
};

int64_t center(const Parameter &parameter) {
    return (parameter.min + parameter.max) / 2;
}

int64_t distance(const Node &node, const std::array<int64_t, 7> &target) {
    int64_t result = 0;
    for (size_t i = 0; i < target.size(); ++i) {
        const int64_t delta = target[i] < node.space[i].min ? node.space[i].min - target[i]
                            : target[i] > node.space[i].max ? target[i] - node.space[i].max : 0;
        result += delta * delta;
    }
    return result;
}

std::shared_ptr<Node> subtree(std::vector<std::shared_ptr<Node>> children) {
    auto node = std::make_shared<Node>();
    node->children = std::move(children);
    for (size_t d = 0; d < node->space.size(); ++d) {
        node->space[d] = node->children.front()->space[d];
        for (size_t i = 1; i < node->children.size(); ++i) {
            node->space[d].min = std::min(node->space[d].min, node->children[i]->space[d].min);
            node->space[d].max = std::max(node->space[d].max, node->children[i]->space[d].max);
        }
    }
    return node;
}

void sort_nodes(std::vector<std::shared_ptr<Node>> &nodes, int first_dimension, bool absolute) {
    std::stable_sort(nodes.begin(), nodes.end(), [=](const auto &a, const auto &b) {
        for (int offset = 0; offset < 7; ++offset) {
            const int d = (first_dimension + offset) % 7;
            int64_t ac = center(a->space[d]);
            int64_t bc = center(b->space[d]);
            if (absolute) {
                ac = std::abs(ac);
                bc = std::abs(bc);
            }
            if (ac != bc) return ac < bc;
        }
        return false;
    });
}

std::vector<std::shared_ptr<Node>> bucketize(const std::vector<std::shared_ptr<Node>> &nodes) {
    size_t expected = 1;
    while (expected * 6 < nodes.size()) expected *= 6;
    std::vector<std::shared_ptr<Node>> buckets;
    for (size_t begin = 0; begin < nodes.size(); begin += expected) {
        const size_t end = std::min(nodes.size(), begin + expected);
        buckets.push_back(subtree({nodes.begin() + static_cast<std::ptrdiff_t>(begin),
                                   nodes.begin() + static_cast<std::ptrdiff_t>(end)}));
    }
    return buckets;
}

int64_t cost(const Node &node) {
    int64_t result = 0;
    for (const auto &parameter : node.space) result += std::abs(parameter.max - parameter.min);
    return result;
}

std::shared_ptr<Node> build_tree(std::vector<std::shared_ptr<Node>> nodes) {
    if (nodes.size() == 1) return nodes.front();
    if (nodes.size() <= 6) {
        std::stable_sort(nodes.begin(), nodes.end(), [](const auto &a, const auto &b) {
            int64_t am = 0, bm = 0;
            for (int d = 0; d < 7; ++d) {
                am += std::abs(center(a->space[d]));
                bm += std::abs(center(b->space[d]));
            }
            return am < bm;
        });
        return subtree(std::move(nodes));
    }

    int64_t best_cost = std::numeric_limits<int64_t>::max();
    int best_dimension = -1;
    std::vector<std::shared_ptr<Node>> best_buckets;
    for (int d = 0; d < 7; ++d) {
        auto sorted = nodes;
        sort_nodes(sorted, d, false);
        auto buckets = bucketize(sorted);
        int64_t total = 0;
        for (const auto &bucket : buckets) total += cost(*bucket);
        if (total < best_cost) {
            best_cost = total;
            best_dimension = d;
            best_buckets = std::move(buckets);
        }
    }
    sort_nodes(best_buckets, best_dimension, true);
    std::vector<std::shared_ptr<Node>> children;
    children.reserve(best_buckets.size());
    for (auto &bucket : best_buckets) children.push_back(build_tree(std::move(bucket->children)));
    return subtree(std::move(children));
}

std::shared_ptr<Node> make_tree() {
    std::vector<std::shared_ptr<Node>> leaves;
    leaves.reserve(std::size(kParameterPoints));
    for (const auto &point : kParameterPoints) {
        auto node = std::make_shared<Node>();
        node->space = point.space;
        node->leaf = &point;
        leaves.push_back(std::move(node));
    }
    return build_tree(std::move(leaves));
}

int64_t point_distance(const ParameterPoint &point, const std::array<int64_t, 7> &target) {
    Node node;
    node.space = point.space;
    return distance(node, target);
}

const ParameterPoint *search_node(const Node &node, const std::array<int64_t, 7> &target,
                                  const ParameterPoint *candidate) {
    if (node.leaf) return node.leaf;
    int64_t best = candidate ? point_distance(*candidate, target) : std::numeric_limits<int64_t>::max();
    for (const auto &child : node.children) {
        const int64_t child_distance = distance(*child, target);
        if (best > child_distance) {
            const ParameterPoint *leaf = search_node(*child, target, candidate);
            if (!leaf) continue;
            const int64_t leaf_distance = child->leaf && child->leaf == leaf ? child_distance : point_distance(*leaf, target);
            if (best > leaf_distance) {
                best = leaf_distance;
                candidate = leaf;
            }
        }
    }
    return candidate;
}

} // namespace

struct Worldgen26::Impl {
    explicit Impl(int64_t seed)
        : sampler(ss_cubiomes_create(seed), ss_cubiomes_destroy), root(make_tree()) {
        if (!sampler) throw std::bad_alloc();
    }

    std::unique_ptr<ss_cubiomes_sampler, decltype(&ss_cubiomes_destroy)> sampler;
    std::shared_ptr<Node> root;
    const ParameterPoint *last = nullptr;
};

SpawnWeight spawn_weight(BiomeProfile profile) {
    switch (profile) {
    case BiomeProfile::common: return {400, 515};
    case BiomeProfile::old_growth_pine: return {400, 540};
    case BiomeProfile::jungle: return {400, 517};
    case BiomeProfile::ocean: return {400, 520};
    case BiomeProfile::frozen_river: return {400, 516};
    case BiomeProfile::river: return {400, 615};
    case BiomeProfile::dripstone: return {400, 610};
    case BiomeProfile::swamp: return {401, 516};
    case BiomeProfile::sulfur: return {25, 311};
    case BiomeProfile::empty: return {0, 0};
    }
    return {0, 0};
}

double spawn_ratio(BiomeProfile profile) {
    const auto weight = spawn_weight(profile);
    return weight.monster_weight ? static_cast<double>(weight.slime_group_weight) / weight.monster_weight : 0.0;
}

uint32_t spawn_ratio_q32(BiomeProfile profile) {
    const auto weight = spawn_weight(profile);
    if (!weight.monster_weight) return 0;
    const uint64_t scaled = static_cast<uint64_t>(weight.slime_group_weight) << 32;
    return static_cast<uint32_t>((scaled + weight.monster_weight / 2) / weight.monster_weight);
}

Worldgen26::Worldgen26(int64_t seed) : impl_(std::make_unique<Impl>(seed)) {}
Worldgen26::~Worldgen26() = default;
Worldgen26::Worldgen26(Worldgen26 &&) noexcept = default;
Worldgen26 &Worldgen26::operator=(Worldgen26 &&) noexcept = default;

BiomeProfile Worldgen26::biome_at_block(int32_t x, int32_t y, int32_t z) {
    int64_t climate[6];
    ss_cubiomes_climate(impl_->sampler.get(), x, y, z, climate);
    std::array<int64_t, 7> target{climate[0], climate[1], climate[2], climate[3], climate[4], climate[5], 0};

    impl_->last = search_node(*impl_->root, target, impl_->last);
    return impl_->last->profile;
}

} // namespace ss::worldgen26
