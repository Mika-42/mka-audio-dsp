module;

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
export module mka.audio.dsp.dag;
export import mka.audio.dsp.error;
export import mka.audio.dsp.node;
export import mka.audio.dsp.connection;
export import mka.audio.dsp.execution_plan;
export import mka.audio.dsp.topology;

export namespace mka::audio::dsp::detail {
    /**
     * @brief Control-only, two-phase monotonic identity sequence used by DAG.
     * @details The wide cursor represents UINT32_MAX + 1 without wrapping.
     * candidate() never consumes an identity; commit() follows successful
     * insertion only. No allocation, Node ownership or synchronization.
     * The initial cursor permits direct tests of the real 32-bit boundary;
     * every DAG starts its private sequence at one.
     */
    class NodeIDSequence {
    public:
        explicit NodeIDSequence(const std::uint64_t first = 1) noexcept : next_(first) {}

        [[nodiscard]] std::expected<NodeID, GraphError> candidate() const noexcept {
            if (next_ == invalidNodeID || next_ > maxNodeID)
                return std::unexpected(GraphError::NodeIDExhausted);
            return static_cast<NodeID>(next_);
        }

        void commit() noexcept {
            if (next_ != invalidNodeID && next_ <= maxNodeID) ++next_;
        }

    private:
        std::uint64_t next_; ///< Control-owned next candidate, never narrowed unchecked.
    };
}

// non-RT graph
namespace mka::audio::dsp {
    export class DAG;

    /** Control-only total order; no graph mutation or audio processing. */
    export [[nodiscard]] std::expected<std::vector<NodeID>, GraphError>
    topologicalOrder(const DAG& graph);

    /**
     * @brief Final control-side owner of stable-address Node objects.
     * @details All edits, compilation, retirement collection and destruction
     * are externally serialized on control. The owner outlives every plan and
     * borrowed pointer. Growing/reordering unique_ptr storage never moves a Node.
     * Plans hold raw Node pointers and borrow tokens, not Node ownership.
     */
    export class DAG final {
    public:
        DAG() = default;
        /** Maximum simultaneously owned Nodes, including deferred retirements. */
        explicit DAG(const std::size_t maxOwnedNodes) noexcept : maxOwnedNodes_(maxOwnedNodes) {}
        DAG(const DAG&) = delete;
        DAG& operator=(const DAG&) = delete;
        // Keep ownership stable while plans or external pointers borrow Nodes.
        DAG(DAG&&) = delete;
        DAG& operator=(DAG&&) = delete;

        /**
         * @brief Construct T(assigned NodeID, forwarded args...) transactionally.
         * @return A unique, nonzero 32-bit ID, or a structured control-side error.
         * @details Construction/allocation failures leave Nodes, plans and the
         * identity sequence unchanged. IDs are never reused during this DAG's
         * lifetime, including after removal. The quota includes retired Nodes.
         * @pre T obeys Node's configuration/lifetime contract. Its constructor
         * must not reenter this DAG, publish a provisional ID/pointer, or mutate
         * an active Node. Arbitrary constructor side effects cannot be rolled back.
         */
        template<class T, class... Args>
            requires std::derived_from<T, Node> && (!std::is_abstract_v<T>) &&
                     std::constructible_from<T, NodeID, Args...>
        [[nodiscard]] std::expected<NodeID, GraphError> add(Args&&... args) noexcept {
            const auto id = identities_.candidate();
            if (!id) return std::unexpected(id.error());
            if (nodes_.size() >= maxOwnedNodes_ ||
                retiredNodes_.size() >= maxOwnedNodes_ - nodes_.size() ||
                nodes_.size() == nodes_.max_size())
                return std::unexpected(GraphError::NodeCapacityExceeded);

            std::unique_ptr<Node> created;
            try {
                created = std::make_unique<T>(static_cast<NodeID>(*id), std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError::AllocationFailed);
            } catch (...) {
                return std::unexpected(GraphError::NodeConstructionFailed);
            }
            if (!created) return std::unexpected(GraphError::AllocationFailed);
            if (created->id() != *id) return std::unexpected(GraphError::InvalidNodeIdentity);
            try {
                nodes_.push_back(std::move(created));
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError::AllocationFailed);
            } catch (const std::length_error&) {
                return std::unexpected(GraphError::NodeCapacityExceeded);
            }
            identities_.commit();
            return *id;
        }

        /**
         * @brief Transactionally replace an unborrowed Node's processed layouts.
         * @return NodeNotFound for an absent identity, NodeInUse while any plan
         * borrows the Node, intrinsic Config validation errors, AllocationFailed
         * for candidate-copy failure or LayoutCapacityExceeded for a size error.
         * @details Control only, externally serialized with graph edits and all
         * configuration readers. Validate and copy before installing by noexcept
         * move. Failure preserves the old Config, edges, roles and existing plans.
         * Logical connections never depend on membership in these layouts, so a
         * successful replacement preserves every Connection and the Node's ID,
         * address and DSP state. No constructor/process or backend call occurs.
         * @pre External views of old layout contents must no longer be used on
         * success. Retire all prepared/structural plans after callback quiescence
         * before retrying NodeInUse; no active-plan configuration is mutated.
         * The caller separately prepares the derived DSP for its new layouts.
         */
        [[nodiscard]] Result setConfig(const NodeID id, const Config& config) try {
            auto* const target = node(id);
            if (target == nullptr) return std::unexpected(GraphError::NodeNotFound);
            if (detail::NodeBorrowAccess::inUse(*target))
                return std::unexpected(GraphError::NodeInUse);
            if (const auto valid = validateConfig(config); !valid) return valid;
            Config candidate = config;
            detail::NodeConfigAccess::replaceConfig(*target, std::move(candidate));
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError::AllocationFailed);
        } catch (const std::length_error&) {
            return std::unexpected(GraphError::LayoutCapacityExceeded);
        }

        /**
         * @brief Remove from the desired graph, deferring destruction if borrowed.
         * @details Secure retirement ownership before changing graph membership,
         * edges or output markers. An old plan can still process the same Node.
         * Allocation failure leaves the desired graph and existing plans intact.
         * Destruction and collection occur only on control, never in a callback.
         * All Ready/Pending/Active/Retired plan owners retain the same borrow
         * protection; one callback ACK cannot retire another generation's borrow.
         * @pre Stop external raw-pointer uses before immediate destruction when
         * no plan borrows the Node, or before collecting its deferred retirement.
         */
        [[nodiscard]] std::expected<void, GraphError> remove(const NodeID nodeId) {
            const auto nodeIt = std::ranges::find(nodes_, nodeId, &Node::id);

            if (nodeIt == nodes_.end()) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            if (detail::NodeBorrowAccess::inUse(**nodeIt)) {
                if (retiredNodes_.size() == retiredNodes_.max_size())
                    return std::unexpected(GraphError::NodeCapacityExceeded);
                try {
                    // unique_ptr moves are noexcept; vector allocation failure
                    // preserves the original owner before any graph edit.
                    retiredNodes_.push_back(std::move(*nodeIt));
                } catch (const std::bad_alloc&) {
                    return std::unexpected(GraphError::AllocationFailed);
                } catch (const std::length_error&) {
                    return std::unexpected(GraphError::NodeCapacityExceeded);
                }
            }

            nodes_.erase(nodeIt);

            std::erase_if(connections_, [nodeId](const Connection &conn) {
                return decodeNode(conn.from) == nodeId || decodeNode(conn.to) == nodeId;
            });
            std::erase(outputsNodes_, nodeId);

            return {};
        }

        /**
         * @brief Destroy retired Nodes no longer borrowed by any plan, on control.
         * @return Number of polymorphic destructions performed by this call.
         * @pre Release external raw-pointer uses and retire plans only after
         * their callbacks quiesce. Untracked external references cannot extend
         * ownership. The DAG must still outlive every remaining plan.
         * Control must retain the active plan until a replacement is installed
         * and its old callback is acknowledged; this is not a publication protocol.
         */
        [[nodiscard]] std::size_t collectRetiredNodes() noexcept {
            return std::erase_if(retiredNodes_, [](const auto& retired) noexcept {
                return !detail::NodeBorrowAccess::inUse(*retired);
            });
        }

        /**
         * @brief Mark an existing Node for direct access to all backend outputs.
         * @details Control-side, idempotent and independent of source status,
         * logical layouts and outgoing edges. Existing plans retain their role
         * snapshot; compile a new plan to apply this edit. IDs are stored once
         * in increasing order. Allocation failure leaves the marker set intact.
         */
        [[nodiscard]] std::expected<void, GraphError> markAsOutput(const NodeID id) try {
            if (!nodeExists(id)) return std::unexpected(GraphError::NodeNotFound);
            const auto position = std::ranges::lower_bound(outputsNodes_, id);
            if (position != outputsNodes_.end() && *position == id) return {};
            outputsNodes_.insert(position, id);
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError::AllocationFailed);
        } catch (const std::length_error&) {
            return std::unexpected(GraphError::NodeCapacityExceeded);
        }

        /**
         * @brief Idempotently remove an existing Node's desired output role.
         * @return NodeNotFound for an absent Node; success even if already unmarked.
         * @details Control only. No allocation, graph edge, channel reservation
         * or change to the roles/buffers of any existing prepared plan.
         */
        [[nodiscard]] std::expected<void, GraphError> unmarkAsOutput(const NodeID id) noexcept {
            if (!nodeExists(id)) return std::unexpected(GraphError::NodeNotFound);
            std::erase(outputsNodes_, id);
            return {};
        }

        /**
         * @brief Read-only desired output identities in increasing NodeID order.
         * @details Control-owned values, not hardware channels. Like connections(),
         * any edit may invalidate this borrowed view. No concurrent edit/read;
         * copy values for an independent snapshot. No Node/buffer ownership.
         */
        [[nodiscard]] std::span<const NodeID> outputNodes() const noexcept {
            return outputsNodes_;
        }

        /**
         * @brief Connect a logical destination In/Aux to a producer Out, on control.
         * @param destination Complete consuming ChannelID (first argument).
         * @param source Complete producing ChannelID (second argument).
         * @details Decode all fields without normalization. Validate ports,
         * direction, Node existence, exact duplicate, input occupancy, then
         * acyclicity in that order. Store Connection{source,destination} only
         * after every check succeeds. Indices need not appear in Config.
         * Fan-out is allowed; this operation never selects backend lanes.
         * @return InvalidPort, InvalidDirection, NodeNotFound, ConnectionExists,
         * InputOccupied, CycleDetected, AllocationFailed or ConnectionCapacityExceeded.
         * @note Control only, externally serialized with graph edits. Rejection
         * leaves desired edges and prepared plans intact. Success edits only
         * the desired graph, not Node DSP state or an existing plan snapshot.
         */
        [[nodiscard]] std::expected<void, GraphError>
        connect(const ChannelID destination, const ChannelID source) try {
            const auto sourcePort = decodePort(source);
            const auto destinationPort = decodePort(destination);
            if (!isValidPort(sourcePort) || !isValidPort(destinationPort)) {
                return std::unexpected(GraphError::InvalidPort);
            }
            if (!isOutputPort(sourcePort) || !isInputPort(destinationPort)) {
                return std::unexpected(GraphError::InvalidDirection);
            }

            const auto sourceNode = decodeNode(source);
            const auto destinationNode = decodeNode(destination);
            if (!nodeExists(sourceNode) || !nodeExists(destinationNode)) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            if (std::ranges::find(connections_, Connection{source, destination}) != connections_.end()) {
                return std::unexpected(GraphError::ConnectionExists);
            }

            if (inputOccupied(destination)) {
                return std::unexpected(GraphError::InputOccupied);
            }

            // A path destination -> source would close a cycle; includes self-loops.
            if (pathExists(destinationNode, sourceNode)) {
                return std::unexpected(GraphError::CycleDetected);
            }

            connections_.emplace_back(source, destination);
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError::AllocationFailed);
        } catch (const std::length_error&) {
            return std::unexpected(GraphError::ConnectionCapacityExceeded);
        }

        /**
         * @brief Checked adapter for legacy wide Channel fields, on control.
         * @details Uses the same destination,source convention and port/error
         * precedence. Check full-width NodeIDs and indices outside [0,65535]
         * before encoding; no wide value may alias an existing identity.
         */
        [[nodiscard]] std::expected<void, GraphError>
        connect(const Channel& destination, const Channel& source) {
            if (!isValidPort(source.port) || !isValidPort(destination.port))
                return std::unexpected(GraphError::InvalidPort);
            if (!isOutputPort(source.port) || !isInputPort(destination.port))
                return std::unexpected(GraphError::InvalidDirection);
            if (!nodeExists(source.nodeID) || !nodeExists(destination.nodeID))
                return std::unexpected(GraphError::NodeNotFound);
            const auto sourceIndex = toChannelIndex(source.channel);
            if (!sourceIndex) return std::unexpected(sourceIndex.error());
            const auto destinationIndex = toChannelIndex(destination.channel);
            if (!destinationIndex) return std::unexpected(destinationIndex.error());
            // Existence was checked on the full-width values before narrowing.
            return connect(encode(static_cast<NodeID>(destination.nodeID), destination.port, *destinationIndex),
                           encode(static_cast<NodeID>(source.nodeID), source.port, *sourceIndex));
        }

        /**
         * @brief Read-only view of desired logical edge values, on control.
         * @details DAG owns the descriptors. Any graph edit may invalidate the
         * span/element references; copy values to retain an independent snapshot.
         * This view owns no Nodes or samples, is not stored by a prepared plan,
         * and must not be read concurrently with edits. No allocation or lock.
         */
        [[nodiscard]] std::span<const Connection> connections() const noexcept {
            return connections_;
        }

        /**
         * @brief Remove exactly source Out -> destination In/Aux, on control.
         * @return true if erased, false if already absent; InvalidDirection for
         * malformed/forbidden port pairs, NodeNotFound for missing endpoints.
         * @details Idempotent for valid live endpoints. Both complete ChannelIDs
         * identify the edge; other channels/ports of the same Nodes are untouched.
         * No Config lookup, allocation, Node mutation or prepared-plan change.
         * @pre Serialize desired-graph edits externally. Any old connections()
         * view may be invalidated; plans own their prepared binding snapshot.
         */
        [[nodiscard]] std::expected<bool, GraphError>
        disconnect(const ChannelID destination, const ChannelID source) {
            if (!isOutputPort(decodePort(source)) || !isInputPort(decodePort(destination)))
                return std::unexpected(GraphError::InvalidDirection);
            if (!nodeExists(decodeNode(source)) || !nodeExists(decodeNode(destination)))
                return std::unexpected(GraphError::NodeNotFound);
            const auto edgeIt = std::ranges::find(connections_, Connection{source, destination});
            if (edgeIt == connections_.end()) return false;
            connections_.erase(edgeIt);
            return true;
        }

        /**
         * @brief Borrow a desired Node, or nullptr for absent/retired identities.
         * @details Look up on control; no allocation/lock. The address survives
         * insertions and unrelated removals. Mutable DSP access requires
         * quiescence if a plan processes this Node. No lifetime is transferred.
         */
        [[nodiscard]] Node* node(const NodeID id) noexcept {
            const auto it = std::ranges::find(nodes_, id, &Node::id);
            return it == nodes_.end() ? nullptr : it->get();
        }

        /// Read-only borrowed lookup, with the same lifetime contract as node().
        [[nodiscard]] const Node *node(const NodeID id) const noexcept {
            const auto it = std::ranges::find(nodes_, id, &Node::id);

            if (it == nodes_.end()) {
                return nullptr;
            }

            return it->get();
        }

        /**
         * @brief Compile desired structure into stable, borrowed Node tasks on control.
         * @details No Node::process, backend access or audio buffer preparation.
         * Temporary structural plans support inspection of desired membership
         * and serial execution steps. Resource errors are returned on control.
         * The owner must outlive the plan; retire task/layer views with it. Use the
         * BackendConfig overload to prepare an executable audio snapshot.
         */
        [[nodiscard]] std::expected<ExecutionPlan, ErrorType> compile() const try {
            std::vector<std::size_t> layerIndices;

            auto sortedNodes = topologicalSort(layerIndices);

            if (!sortedNodes) {
                return std::unexpected(sortedNodes.error());
            }

            return ExecutionPlan {
                std::move(*sortedNodes),
                std::move(layerIndices)
            };
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError::AllocationFailed);
        } catch (const std::length_error&) {
            return std::unexpected(GraphError::PreparationCapacityExceeded);
        }

        /**
         * @brief Compile and prepare direct backend I/O on control, before RT.
         * @details Each logical output has distinct plan-owned storage. This
         * baseline does not recycle slots or implement PDC/publication.
         */
        [[nodiscard]] std::expected<ExecutionPlan, ErrorType>
        compile(const BackendConfig& config) const {
            auto plan = compile();
            if (!plan) return std::unexpected(plan.error());
            if (const auto result = plan->prepareBackend(config, connections_, outputsNodes_); !result) {
                return std::unexpected(result.error());
            }
            return plan;
        }

    private:
        friend std::expected<std::vector<NodeID>, GraphError> topologicalOrder(const DAG& graph);

        /** Owned control snapshot; no Node addresses escape through its values. */
        [[nodiscard]] detail::GraphSnapshot structuralSnapshot() const {
            detail::GraphSnapshot snapshot;
            snapshot.nodes.reserve(nodes_.size());
            for (const auto& node : nodes_) snapshot.nodes.push_back(node->id());
            snapshot.connections = connections_;
            snapshot.outputs = outputsNodes_;
            return snapshot;
        }

        /**
         * @brief Full-ChannelID occupancy, derived from desired edges on control.
         * @details In/Aux and indices are independent, including outside Config.
         * No separate index can become stale after disconnect/remove. Scan only;
         * no allocation, ownership, callback access or implicit fan-in/mixing.
         */
        [[nodiscard]] bool inputOccupied(const ChannelID destination) const noexcept {
            return std::ranges::any_of(connections_, [destination](const Connection& edge) {
                return edge.to == destination;
            });

        }

        [[nodiscard]] bool nodeExists(const std::uint64_t id) const {
            const auto nodeIt = std::ranges::find(nodes_, id, &Node::id);
            return nodeIt != nodes_.end();
        }

        /**
         * @brief Iterative reachability on control, including all In/Aux edges.
         * @details Build adjacency once, then visit each reachable Node/edge at
         * most once: O(V+E) average with integer hash tables, O(V+E) temporary
         * memory. No recursive call stack or Config filtering. Parallel channel
         * edges are harmless; discovered Nodes are queued only once. Allocation
         * and length failures propagate to connect's structured error boundary.
         * Graph/Nodes/plans are read-only; all temporary storage dies on control.
         */
        [[nodiscard]] bool pathExists(const NodeID src, const NodeID dst) const {
            if (src == dst)
                return true;

            std::unordered_map<NodeID, std::vector<NodeID>> successors;
            successors.reserve(nodes_.size());
            for (const auto& edge : connections_)
                successors[decodeNode(edge.from)].push_back(decodeNode(edge.to));
            std::vector<NodeID> stack;
            stack.reserve(nodes_.size());
            std::unordered_set<NodeID> visited;
            visited.reserve(nodes_.size());
            visited.insert(src);
            stack.push_back(src);

            while (!stack.empty()) {
                const NodeID current = stack.back();
                stack.pop_back();
                const auto found = successors.find(current);
                if (found == successors.end()) continue;
                for (const auto next : found->second) {
                    if (next == dst) return true;
                    if (visited.insert(next).second) stack.push_back(next);
                }
            }

            return false;
        }

        /**
         * @brief Map the total min-heap order to borrowed stable task pointers.
         * @param[out] layerIndices Unit serial-step boundaries, modified only
         * on success. These are not breadth-first dependency levels: a newly
         * available low ID may precede an old frontier member.
         * @note Empty graph gives no tasks and indices {0}. Allocations occur
         * on control and are caught by compile(). No parallel scheduling here.
         * @pre Serialize edits; the Node owner must outlive the resulting plan.
         */
        [[nodiscard]] std::expected<std::vector<Node*>, ErrorType>
        topologicalSort(std::vector<std::size_t>& layerIndices) const {
            const auto order = topologicalOrder(*this);
            if (!order) return std::unexpected(order.error());
            std::unordered_map<NodeID, Node*> addresses;
            addresses.reserve(nodes_.size());
            for (const auto& node : nodes_) addresses.emplace(node->id(), node.get());
            std::vector<Node*> result;
            result.reserve(order->size());
            std::vector<std::size_t> offsets;
            if (order->size() >= offsets.max_size())
                return std::unexpected(GraphError::PreparationCapacityExceeded);
            offsets.reserve(order->size() + 1);
            offsets.push_back(0);
            for (const auto id : *order) {
                const auto found = addresses.find(id);
                if (found == addresses.end()) return std::unexpected(GraphError::NodeNotFound);
                result.push_back(found->second);
                offsets.push_back(result.size());
            }
            layerIndices = std::move(offsets);
            return result;
        }

        // Sole control-owned storage; only unique_ptr handles move, never Nodes.
        std::vector<std::unique_ptr<Node> > nodes_;
        // Removed Nodes remain owned until every plan borrow retires on control.
        std::vector<std::unique_ptr<Node>> retiredNodes_;
        std::vector<Connection> connections_; ///< Control-owned logical values, no sample pointers.
        std::vector<NodeID> outputsNodes_;
        detail::NodeIDSequence identities_; ///< Committed IDs never become available again.
        std::size_t maxOwnedNodes_ = maxNodeID; ///< Live + retired quota; immutable after construction.
    };

    [[nodiscard]] std::expected<std::vector<NodeID>, GraphError>
    topologicalOrder(const DAG& graph) try {
        return detail::topologicalOrder(graph.structuralSnapshot());
    } catch (const std::bad_alloc&) {
        return std::unexpected(GraphError::AllocationFailed);
    } catch (const std::length_error&) {
        return std::unexpected(GraphError::PreparationCapacityExceeded);
    }
}
