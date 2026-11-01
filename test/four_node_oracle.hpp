#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Independent test model, with no production imports or validation helpers.
 * Values and vectors belong to the test on control; nothing reaches audio.
 */
namespace four_node_oracle {
    using NodeId = std::uint32_t;
    using ChannelId = std::uint64_t;
    inline constexpr std::uint16_t in = 0, out = 1, aux = 2;
    inline constexpr std::size_t nodeCount = 4;
    inline constexpr unsigned maskCount = 1U << 12U;

    struct Edge {
        ChannelId from{};
        ChannelId to{};
        constexpr bool operator==(const Edge&) const noexcept = default;
    };

    enum class Decision : std::uint16_t {
        Accepted, InvalidPort, InvalidDirection, NodeNotFound,
        ConnectionExists, InputOccupied, CycleDetected
    };

    using Closure = std::array<std::array<bool, nodeCount>, nodeCount>;

    [[nodiscard]] constexpr ChannelId channel(NodeId node, std::uint16_t port,
                                               std::uint16_t index) noexcept {
        return (ChannelId{node} << 32U) | (ChannelId{port} << 16U) | ChannelId{index};
    }
    [[nodiscard]] constexpr NodeId nodeOf(ChannelId id) noexcept {
        return static_cast<NodeId>(id >> 32U);
    }
    [[nodiscard]] constexpr std::uint16_t portOf(ChannelId id) noexcept {
        return static_cast<std::uint16_t>((id >> 16U) & ChannelId{0xFFFF});
    }

    struct Graph {
        std::array<NodeId, nodeCount> nodes{1, 2, 3, 4};
        std::vector<Edge> edges;

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

        [[nodiscard]] bool acyclic() const noexcept {
            const auto reach = closure();
            for (std::size_t i = 0; i < nodeCount; ++i)
                if (reach[i][i]) return false;
            return true;
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
