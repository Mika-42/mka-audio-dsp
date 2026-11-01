module;
#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <new>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

export module mka.audio.dsp.topology;
export import mka.audio.dsp.graph_snapshot;

export namespace mka::audio::dsp::detail {
    /**
     * @brief Lexicographically smallest topological NodeID order, on control.
     * @details Kahn's algorithm chooses the least currently available NodeID,
     * including isolated Nodes and Nodes newly released by the last extraction.
     * Every logical In/Aux edge contributes one indegree decrement; parallel
     * relations on different channels are handled exactly. No Config lookup or
     * mutation of the owned input values.
     * @return InvalidNodeIdentity for zero/duplicate listed identities,
     * InvalidPort/InvalidDirection for malformed edges, NodeNotFound for an
     * unlisted endpoint, CycleDetected if extraction omits any listed Node,
     * AllocationFailed/PreparationCapacityExceeded for temporary storage errors.
     * @note O((V+E) log V) average with integer hash lookup and O(V+E) memory.
     * No Node pointer, ownership operation, allocation or traversal reaches RT.
     * This helper orders dependencies; it does not validate destination
     * occupancy, output-role membership or DSP Configs.
     */
    [[nodiscard]] std::expected<std::vector<NodeID>, GraphError>
    topologicalOrder(const GraphSnapshot& graph) try {
        const auto count = graph.nodes.size();
        std::unordered_map<NodeID, std::size_t> positions;
        positions.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (graph.nodes[i] == invalidNodeID || !positions.emplace(graph.nodes[i], i).second)
                return std::unexpected(GraphError::InvalidNodeIdentity);
        }
        std::vector<std::vector<std::size_t>> successors(count);
        std::vector<std::size_t> remaining(count, 0);
        for (const auto& edge : graph.connections) {
            const auto sourcePort = decodePort(edge.from);
            const auto destinationPort = decodePort(edge.to);
            if (!isValidPort(sourcePort) || !isValidPort(destinationPort))
                return std::unexpected(GraphError::InvalidPort);
            if (!isOutputPort(sourcePort) || !isInputPort(destinationPort))
                return std::unexpected(GraphError::InvalidDirection);
            const auto source = positions.find(decodeNode(edge.from));
            const auto destination = positions.find(decodeNode(edge.to));
            if (source == positions.end() || destination == positions.end())
                return std::unexpected(GraphError::NodeNotFound);
            if (remaining[destination->second] == std::numeric_limits<std::size_t>::max())
                return std::unexpected(GraphError::PreparationCapacityExceeded);
            successors[source->second].push_back(destination->second);
            ++remaining[destination->second];
        }
        std::vector<NodeID> readyStorage;
        readyStorage.reserve(count);
        std::priority_queue<NodeID, std::vector<NodeID>, std::greater<NodeID>> ready{
            std::greater<NodeID>{}, std::move(readyStorage)};
        for (std::size_t i = 0; i < count; ++i)
            if (remaining[i] == 0) ready.push(graph.nodes[i]);
        std::vector<NodeID> result;
        result.reserve(count);
        while (!ready.empty()) {
            const auto id = ready.top();
            ready.pop();
            result.push_back(id);
            const auto index = positions.find(id)->second;
            for (const auto destination : successors[index])
                if (--remaining[destination] == 0) ready.push(graph.nodes[destination]);
        }
        if (result.size() != count) return std::unexpected(GraphError::CycleDetected);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(GraphError::AllocationFailed);
    } catch (const std::length_error&) {
        return std::unexpected(GraphError::PreparationCapacityExceeded);
    }
}
