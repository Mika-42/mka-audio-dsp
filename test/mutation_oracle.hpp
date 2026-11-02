#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Independent mutation model used only by control-side tests.
 * No production imports, encoders, validators, traversal or storage indexes.
 * The test bounds live membership to twelve Nodes and owns every value here;
 * this model neither borrows Node addresses nor touches audio sample storage.
 */
namespace mutation_oracle {
    using NodeId = std::uint32_t;
    using ChannelId = std::uint64_t;
    inline constexpr std::uint16_t in = 0, out = 1, aux = 2;
    inline constexpr std::size_t maxLive = 12;

    enum class Decision : std::uint16_t {
        Accepted, InvalidPort, InvalidDirection, NodeNotFound,
        ConnectionExists, InputOccupied, CycleDetected
    };
    enum class Kind : std::uint16_t { Add, Connect, Disconnect, Remove, Mark, Unmark };

    struct Edge {
        ChannelId from{};
        ChannelId to{};
        constexpr bool operator==(const Edge&) const noexcept = default;
    };
    struct Operation {
        Kind kind;
        NodeId id{};
        ChannelId source{};
        ChannelId destination{};
    };
    struct Outcome {
        Decision decision{Decision::Accepted};
        bool value = false; ///< Exact presence/removal for Disconnect.
        NodeId id = 0;      ///< Proposed identity for Add, not a published Node.
    };

    [[nodiscard]] constexpr ChannelId channel(NodeId node, std::uint16_t port,
                                               std::uint16_t index) noexcept {
        return (ChannelId{node} << 32U) | (ChannelId{port} << 16U) | ChannelId{index};
    }
    [[nodiscard]] constexpr NodeId nodeOf(ChannelId value) noexcept {
        return static_cast<NodeId>(value >> 32U);
    }
    [[nodiscard]] constexpr std::uint16_t portOf(ChannelId value) noexcept {
        return static_cast<std::uint16_t>((value >> 16U) & ChannelId{0xFFFF});
    }
    [[nodiscard]] constexpr std::uint16_t indexOf(ChannelId value) noexcept {
        return static_cast<std::uint16_t>(value & ChannelId{0xFFFF});
    }

    struct Model {
        using Closure = std::array<std::array<bool, maxLive>, maxLive>;

        NodeId nextID = 1;
        std::vector<NodeId> nodes;
        std::vector<Edge> edges;
        std::vector<NodeId> outputs;

        [[nodiscard]] bool contains(NodeId id) const noexcept {
            return std::find(nodes.begin(), nodes.end(), id) != nodes.end();
        }

        [[nodiscard]] std::size_t position(NodeId id) const noexcept {
            const auto found = std::find(nodes.begin(), nodes.end(), id);
            return static_cast<std::size_t>(found - nodes.begin());
        }

        /**
         * Floyd-Warshall over model membership, independent of production DFS.
         * @pre The generated scenario has at most maxLive live identities and
         * every stored reference edge has two live endpoints.
         */
        [[nodiscard]] Closure closure() const noexcept {
            assert(nodes.size() <= maxLive);
            Closure reach{};
            for (const auto& edge : edges) {
                const auto source = position(nodeOf(edge.from));
                const auto destination = position(nodeOf(edge.to));
                assert(source < nodes.size() && destination < nodes.size());
                if (source < nodes.size() && destination < nodes.size())
                    reach[source][destination] = true;
            }
            for (std::size_t via = 0; via < nodes.size(); ++via)
                for (std::size_t source = 0; source < nodes.size(); ++source)
                    for (std::size_t destination = 0; destination < nodes.size(); ++destination)
                        reach[source][destination] = reach[source][destination] ||
                            (reach[source][via] && reach[via][destination]);
            return reach;
        }

        [[nodiscard]] bool acyclic() const noexcept {
            const auto reachable = closure();
            for (std::size_t i = 0; i < nodes.size(); ++i)
                if (reachable[i][i]) return false;
            return true;
        }

        /** No mutation or production call participates in this prediction. */
        [[nodiscard]] Outcome predict(const Operation& operation) const noexcept {
            const auto source = nodeOf(operation.source);
            const auto destination = nodeOf(operation.destination);
            switch (operation.kind) {
                case Kind::Add:
                    // The bounded test generator reroutes Add at maxLive and
                    // performs at most 10,000 additions, far from uint32 wrap.
                    assert(nodes.size() < maxLive && nextID != 0);
                    return {Decision::Accepted, false, nextID};
                case Kind::Connect: {
                    const auto sourcePort = portOf(operation.source);
                    const auto destinationPort = portOf(operation.destination);
                    if (sourcePort > aux || destinationPort > aux)
                        return {Decision::InvalidPort};
                    if (sourcePort != out || (destinationPort != in && destinationPort != aux))
                        return {Decision::InvalidDirection};
                    if (!contains(source) || !contains(destination))
                        return {Decision::NodeNotFound};
                    const Edge candidate{operation.source, operation.destination};
                    if (std::find(edges.begin(), edges.end(), candidate) != edges.end())
                        return {Decision::ConnectionExists};
                    for (const auto& edge : edges)
                        if (edge.to == operation.destination) return {Decision::InputOccupied};
                    if (source == destination || closure()[position(destination)][position(source)])
                        return {Decision::CycleDetected};
                    return {Decision::Accepted, true};
                }
                case Kind::Disconnect: {
                    const auto sourcePort = portOf(operation.source);
                    const auto destinationPort = portOf(operation.destination);
                    // This API intentionally groups malformed ports with wrong
                    // direction, unlike Connect's separate InvalidPort result.
                    if (sourcePort != out || (destinationPort != in && destinationPort != aux))
                        return {Decision::InvalidDirection};
                    if (!contains(source) || !contains(destination))
                        return {Decision::NodeNotFound};
                    const Edge candidate{operation.source, operation.destination};
                    return {Decision::Accepted,
                            std::find(edges.begin(), edges.end(), candidate) != edges.end()};
                }
                case Kind::Remove:
                    return contains(operation.id) ? Outcome{Decision::Accepted, true}
                                                  : Outcome{Decision::NodeNotFound};
                case Kind::Mark:
                    if (!contains(operation.id)) return {Decision::NodeNotFound};
                    return {Decision::Accepted,
                            std::find(outputs.begin(), outputs.end(), operation.id) == outputs.end()};
                case Kind::Unmark:
                    if (!contains(operation.id)) return {Decision::NodeNotFound};
                    return {Decision::Accepted,
                            std::find(outputs.begin(), outputs.end(), operation.id) != outputs.end()};
            }
            assert(false && "Unknown reference operation");
            return {Decision::InvalidDirection};
        }

        /**
         * Apply only the independently predicted valid transition.
         * Allocations and reference values are owned by the control test. No
         * rejected operation changes even the identity sequence or edge order.
         */
        Outcome apply(const Operation& operation) {
            const auto outcome = predict(operation);
            if (outcome.decision != Decision::Accepted) return outcome;
            switch (operation.kind) {
                case Kind::Add:
                    nodes.push_back(outcome.id);
                    ++nextID;
                    break;
                case Kind::Connect:
                    edges.push_back({operation.source, operation.destination});
                    break;
                case Kind::Disconnect:
                    if (outcome.value)
                        std::erase(edges, Edge{operation.source, operation.destination});
                    break;
                case Kind::Remove:
                    std::erase(nodes, operation.id);
                    std::erase_if(edges, [&](const Edge& edge) {
                        return nodeOf(edge.from) == operation.id || nodeOf(edge.to) == operation.id;
                    });
                    std::erase(outputs, operation.id);
                    break;
                case Kind::Mark:
                    if (outcome.value) {
                        outputs.push_back(operation.id);
                        std::sort(outputs.begin(), outputs.end());
                    }
                    break;
                case Kind::Unmark:
                    std::erase(outputs, operation.id);
                    break;
            }
            return outcome;
        }
    };
}
