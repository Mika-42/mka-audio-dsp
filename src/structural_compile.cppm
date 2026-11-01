module;
#include <cstddef>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

export module mka.audio.dsp.structural_compile;
export import mka.audio.dsp.compile_config;
export import mka.audio.dsp.execution_plan;
export import mka.audio.dsp.topology;

export namespace mka::audio::dsp::detail {
    /**
     * @brief Checked requested bytes for a compiled structural plan, on control.
     * @details Counts N Node pointers, N borrow tokens, N ExecutionTask records,
     * N identity/index pairs, two role-index arrays with requested capacity N,
     * and N+1 serial layer offsets. Excludes vector objects, allocator overhead,
     * temporary compilation storage and separately prepared PCM/descriptors.
     * This is a deterministic metadata budget, not a process RSS limit.
     * @return ArithmeticOverflow if this byte calculation cannot fit size_t.
     */
    [[nodiscard]] std::expected<std::size_t, CompileError>
    structuralStorageBytes(const std::size_t count) noexcept {
        constexpr auto perNode = sizeof(Node*) + sizeof(NodeBorrowToken) +
            sizeof(ExecutionTask) + sizeof(std::pair<NodeID, std::size_t>) +
            3 * sizeof(std::size_t);
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        if (count > (maximum - sizeof(std::size_t)) / perNode)
            return std::unexpected(Error{ErrorCode::ArithmeticOverflow});
        return count * perNode + sizeof(std::size_t);
    }

    /**
     * @brief Validate and compile detached desired-graph values transactionally.
     * @param owners Stable, non-null borrowed Node addresses with unique IDs.
     * @param graph Owned structural values; listed IDs select the desired Nodes.
     * @param target Inactive control-owned destination, replaced only on success.
     * @param config Structural metadata capacities and requested byte budget.
     * @details Validates intrinsic Configs, complete logical channel endpoints,
     * destination occupancy, output membership and acyclicity. Every In/Aux
     * edge participates even if its channel index is absent from Config. Output
     * membership is independent of successors and source status. Builds the
     * global lexicographic topology and a complete candidate before its noexcept
     * installation. Failure preserves all target metadata, buffers and borrows.
     * No process(), latency query, backend access or PCM preparation occurs.
     * @pre Serialize all edits and compile/retire only after target quiescence;
     * each selected Node owner outlives the resulting plan and borrowed views.
     * This internal fixture boundary never owns or validates pointer lifetimes.
     */
    [[nodiscard]] Result compileSnapshot(const std::span<Node* const> owners,
        const GraphSnapshot& graph, ExecutionPlan& target, const CompileConfig& config) try {
        const auto count = graph.nodes.size();
        if (count > config.maxNodes || graph.connections.size() > config.maxConnections)
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        const auto bytes = structuralStorageBytes(count);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes > config.memoryBudgetBytes)
            return std::unexpected(Error{ErrorCode::MemoryBudgetExceeded});
        if (count > std::vector<Node*>{}.max_size() ||
            count >= std::vector<std::size_t>{}.max_size() ||
            count > std::vector<NodeBorrowToken>{}.max_size() ||
            count > std::vector<ExecutionTask>{}.max_size() ||
            count > std::vector<std::pair<NodeID, std::size_t>>{}.max_size())
            return std::unexpected(Error{ErrorCode::CapacityExceeded});

        std::unordered_map<NodeID, Node*> addresses;
        addresses.reserve(owners.size());
        for (auto* const node : owners) {
            if (node == nullptr) return std::unexpected(Error{ErrorCode::InvalidParameter});
            const auto id = node->id();
            if (id == invalidNodeID || !addresses.emplace(id, node).second)
                return std::unexpected(Error{ErrorCode::InvalidParameter, id});
        }
        std::unordered_map<NodeID, std::size_t> positions;
        positions.reserve(count);
        std::vector<Node*> selected;
        selected.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = graph.nodes[i];
            if (id == invalidNodeID || !positions.emplace(id, i).second)
                return std::unexpected(Error{ErrorCode::InvalidParameter, id});
            const auto found = addresses.find(id);
            if (found == addresses.end())
                return std::unexpected(Error{ErrorCode::NodeNotFound, id});
            if (const auto valid = validateConfig(found->second->config()); !valid) {
                auto error = valid.error();
                error.node = id;
                return std::unexpected(error);
            }
            selected.push_back(found->second);
        }

        std::vector<bool> sources(count, true);
        std::vector<bool> outputs(count, false);
        for (const auto id : graph.outputs) {
            const auto found = positions.find(id);
            if (found == positions.end())
                return std::unexpected(Error{ErrorCode::NodeNotFound, id});
            if (outputs[found->second])
                return std::unexpected(Error{ErrorCode::InvalidParameter, id});
            outputs[found->second] = true;
        }
        std::unordered_map<ChannelID, ChannelID> incoming;
        incoming.reserve(graph.connections.size());
        for (const auto& edge : graph.connections) {
            const auto sourcePort = decodePort(edge.from);
            const auto destinationPort = decodePort(edge.to);
            if (!isValidPort(sourcePort) || !isValidPort(destinationPort)) {
                const auto channel = !isValidPort(sourcePort) ? edge.from : edge.to;
                return std::unexpected(Error{ErrorCode::InvalidPort, decodeNode(channel), channel});
            }
            if (!isOutputPort(sourcePort) || !isInputPort(destinationPort)) {
                const auto channel = !isOutputPort(sourcePort) ? edge.from : edge.to;
                return std::unexpected(Error{ErrorCode::InvalidDirection, decodeNode(channel), channel});
            }
            if (!positions.contains(decodeNode(edge.from)))
                return std::unexpected(Error{ErrorCode::NodeNotFound, decodeNode(edge.from), edge.from});
            const auto destination = positions.find(decodeNode(edge.to));
            if (destination == positions.end())
                return std::unexpected(Error{ErrorCode::NodeNotFound, decodeNode(edge.to), edge.to});
            const auto [existing, inserted] = incoming.emplace(edge.to, edge.from);
            if (!inserted)
                return std::unexpected(Error{existing->second == edge.from ?
                    ErrorCode::ConnectionExists : ErrorCode::InputOccupied,
                    decodeNode(edge.to), edge.to});
            sources[destination->second] = false;
        }
        const auto order = topologicalOrder(graph);
        if (!order) return std::unexpected(order.error());
        std::vector<ExecutionTask> tasks;
        tasks.reserve(count);
        for (const auto id : *order) {
            const auto position = positions.find(id)->second;
            tasks.push_back(ExecutionTask{selected[position], id, tasks.size(),
                sources[position], outputs[position]});
        }
        ExecutionPlan candidate(std::move(tasks));
        target = std::move(candidate);
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::MemoryBudgetExceeded});
    } catch (const std::length_error&) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }
}
