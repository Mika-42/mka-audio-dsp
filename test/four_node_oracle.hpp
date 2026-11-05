/**
 * @file four_node_oracle.hpp
 * @brief Test fixtures for four node oracle.
 * @author Mika
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Independent test model, with no production imports or validation helpers.
 * Values and vectors belong to the test on control; nothing reaches audio.
 */
namespace four_node_oracle {
    /// @brief Independent reference-model node identity type.
    using NodeId = std::uint32_t;
    /// @brief Independent reference-model numeric channel identity type.
    using ChannelId = std::uint64_t;
    inline constexpr std::uint16_t in = 0 /**< Raw main-input port code used by the independent model. */,
         out = 1 /**< Raw output port code used by the independent model. */,
         aux = 2 /**< Raw auxiliary port code used by the independent model. */;
    /// @brief Number of vertices in the exhaustive four-node graph corpus.
    inline constexpr std::size_t nodeCount = 4;
    /// @brief Number of directed-edge masks enumerated by the exhaustive corpus.
    inline constexpr unsigned maskCount = 1U << 12U;

    /// @brief Numeric source and destination used by the independent graph model.
    struct Edge {
        /// @brief Complete logical producer identity in this reference edge.
        ChannelId from{};
        /// @brief Complete logical consumer identity in this reference edge.
        ChannelId to{};
        /// @brief Compare all value fields for equality.
        constexpr bool operator==(const Edge&) const noexcept = default;
    };

    /// @brief Expected outcome of an edit in the independent graph model.
    enum class Decision : std::uint16_t {
        /// @brief The independent model accepts the requested graph edit.
        Accepted,
        /// @brief A port code is outside the supported main/output/auxiliary domain.
        InvalidPort,
        /// @brief The proposed edge is not Out to In or Aux.
        InvalidDirection,
        /// @brief A required logical node identity is absent.
        NodeNotFound,
        /// @brief The exact logical connection is already present.
        ConnectionExists,
        /// @brief Another producer already feeds this complete destination channel.
        InputOccupied,
        /// @brief The desired dependency relation contains a cycle.
        CycleDetected

    };

    /// @brief Boolean transitive-reachability matrix used by the independent graph model.
    using Closure = std::array<std::array<bool, nodeCount>, nodeCount>;

    /// @brief Encode the reference model's node, port and index without using production helpers.
    [[nodiscard]] constexpr ChannelId channel(NodeId node, std::uint16_t port,
                                               std::uint16_t index) noexcept {
        return (ChannelId{node} << 32U) | (ChannelId{port} << 16U) | ChannelId{index};
    }
    /// @brief Extract the reference model's node field from a numeric channel identity.
    [[nodiscard]] constexpr NodeId nodeOf(ChannelId id) noexcept {
        return static_cast<NodeId>(id >> 32U);
    }
    /// @brief Extract the reference model's port field without normalizing undefined values.
    [[nodiscard]] constexpr std::uint16_t portOf(ChannelId id) noexcept {
        return static_cast<std::uint16_t>((id >> 16U) & ChannelId{0xFFFF});
    }

    /// @brief Owns a fixture DAG and the node identities used to build its signal paths.
    struct Graph {
        /// @brief Node identities or independently owned node metadata belonging to this snapshot.
        std::array<NodeId, nodeCount> nodes{1, 2, 3, 4};
        /// @brief Complete logical connections retained by the graph snapshot or reference model.
        std::vector<Edge> edges;

        /// @brief Return the model node position, or nodeCount when the identity is absent.
        [[nodiscard]] std::size_t indexOf(NodeId id) const noexcept {
            for (std::size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i] == id) return i;
            return nodes.size();
        }

        /** Floyd-Warshall, independent of the production adjacency DFS/Kahn. */
        [[nodiscard]] Closure closure() const noexcept {
            Closure reach{};
            for (const auto& edge : edges) {
                const auto source = indexOf(nodeOf(edge.from));
                const auto destination = indexOf(nodeOf(edge.to));
                if (source < nodes.size() && destination < nodes.size())
                    reach[source][destination] = true;
            }
            for (std::size_t via = 0; via < nodeCount; ++via)
                for (std::size_t source = 0; source < nodeCount; ++source)
                    for (std::size_t destination = 0; destination < nodeCount; ++destination)
                        reach[source][destination] = reach[source][destination] ||
                            (reach[source][via] && reach[via][destination]);
            return reach;
        }

        /// @brief Report whether the independently computed reachability matrix has no self-reachable
        /// vertex.
        [[nodiscard]] bool acyclic() const noexcept {
            const auto reach = closure();
            for (std::size_t i = 0; i < nodeCount; ++i)
                if (reach[i][i]) return false;
            return true;
        }

        /**
         * @brief Independent order oracle: try the 24 sorted permutations.
         * @details The first permutation placing every producer strictly before
         * its consumer is lexicographically minimal. This neither computes
         * indegrees nor uses the production Kahn/DFS implementation. Returned
         * values belong to the test on control; an invalid/cyclic model gives
         * no order. Parallel channel edges impose the same precedence again.
         */
        [[nodiscard]] std::vector<NodeId> lexicographicOrder() const {
            auto permutation = nodes;
            std::sort(permutation.begin(), permutation.end());
            do {
                bool valid = true;
                for (const auto& edge : edges) {
                    const auto source = std::find(permutation.begin(), permutation.end(), nodeOf(edge.from));
                    const auto destination = std::find(permutation.begin(), permutation.end(), nodeOf(edge.to));
                    if (source == permutation.end() || destination == permutation.end() || source >= destination) {
                        valid = false;
                        break;
                    }
                }
                if (valid) return {permutation.begin(), permutation.end()};
            } while (std::next_permutation(permutation.begin(), permutation.end()));
            return {};
        }

        /** Predict without calling DAG, its decoder, or any production helper. */
        [[nodiscard]] Decision predict(const Edge& candidate) const noexcept {
            const auto sourcePort = portOf(candidate.from);
            const auto destinationPort = portOf(candidate.to);
            if (sourcePort > aux || destinationPort > aux) return Decision::InvalidPort;
            if (sourcePort != out || (destinationPort != in && destinationPort != aux))
                return Decision::InvalidDirection;
            const auto source = indexOf(nodeOf(candidate.from));
            const auto destination = indexOf(nodeOf(candidate.to));
            if (source == nodes.size() || destination == nodes.size()) return Decision::NodeNotFound;
            for (const auto& edge : edges)
                if (edge == candidate) return Decision::ConnectionExists;
            for (const auto& edge : edges)
                if (edge.to == candidate.to) return Decision::InputOccupied;
            if (source == destination || closure()[destination][source]) return Decision::CycleDetected;
            return Decision::Accepted;
        }
    };

    /**
     * Twelve directed non-self relations, ordered source-major, encode a mask.
     * Each producer uses a distinct destination index, allowing all Node DAGs
     * without introducing unrelated occupancy failures in their baseline.
     */
    [[nodiscard]] inline Graph fromMask(std::uint16_t mask) {
        Graph graph;
        unsigned bit = 0;
        for (std::size_t source = 0; source < nodeCount; ++source) {
            for (std::size_t destination = 0; destination < nodeCount; ++destination) {
                if (source == destination) continue;
                if ((mask & (std::uint16_t{1} << bit)) != 0)
                    graph.edges.push_back({channel(graph.nodes[source], out, 0),
                        channel(graph.nodes[destination], in, static_cast<std::uint16_t>(10 + source))});
                ++bit;
            }
        }
        return graph;
    }
}
