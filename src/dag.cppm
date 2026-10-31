module;

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
export module mka.audio.dsp.dag;
export import mka.audio.dsp.error;
export import mka.audio.dsp.node;
import mka.audio.dsp.channel;
export import mka.audio.dsp.execution_plan;

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
    struct Connexion {
        Channel from;
        Channel to;
    };

    [[nodiscard]]
    static bool connexionExists(
        const std::vector<Connexion> &connexions,
        const Channel &from,
        const Channel &to
    ) {
        return std::ranges::any_of(
            connexions,
            [&from, &to](const Connexion &connexion) {
                return connexion.from == from &&
                       connexion.to == to;
            }
        );
    }

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
         * @brief Remove from the desired graph, deferring destruction if borrowed.
         * @details Secure retirement ownership before changing graph membership,
         * edges or output markers. An old plan can still process the same Node.
         * Allocation failure leaves the desired graph and existing plans intact.
         * Destruction and collection occur only on control, never in a callback.
         * @pre Stop external raw-pointer uses before immediate destruction when
         * no plan borrows the Node, or before collecting its deferred retirement.
         */
        [[nodiscard]] Result remove(const std::uint64_t nodeId) {
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

            std::erase_if(connexions_, [nodeId](const Connexion &conn) {
                return conn.from.nodeID == nodeId || conn.to.nodeID == nodeId;
            });
            std::erase(outputsNodes_, static_cast<NodeID>(nodeId));

            return {};
        }

        /**
         * @brief Destroy retired Nodes no longer borrowed by any plan, on control.
         * @return Number of polymorphic destructions performed by this call.
         * @pre Release external raw-pointer uses and retire plans only after
         * their callbacks quiesce. Untracked external references cannot extend
         * ownership. The DAG must still outlive every remaining plan.
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
         * snapshot; compile a new plan to apply this edit.
         */
        [[nodiscard]] std::expected<void, GraphError> markAsOutput(const NodeID id) {
            if (!nodeExists(id)) return std::unexpected(GraphError::NodeNotFound);
            if (std::ranges::find(outputsNodes_, id) == outputsNodes_.end()) {
                outputsNodes_.push_back(id);
            }
            return {};
        }

        /**
         * @brief Connect numeric logical channel identities from Node::channel.
         * @details Decode without normalizing unknown port codes, then reuse the
         * existing graph checks. Storage remains the historical Channel value.
         */
        [[nodiscard]] std::expected<void, GraphError>
        connect(const ChannelID from, const ChannelID to) {
            return connect(
                Channel{.nodeID = decodeNode(from), .channel = decodeChannel(from), .port = decodePort(from)},
                Channel{.nodeID = decodeNode(to), .channel = decodeChannel(to), .port = decodePort(to)}
            );
        }

        /**
         * @brief Connect an Out source to an In or Aux destination, on control.
         * @return GraphError::InvalidDirection for every other direction or
         * undefined port code, before checking node existence, cycles or input
         * occupancy. For valid directions, existing graph errors are preserved.
         * @note Rejection leaves the graph unchanged. This potentially allocating
         * control operation is not called from the audio callback; the port
         * helpers themselves are pure constexpr/noexcept comparisons.
         */
        [[nodiscard]] std::expected<void, GraphError>
        connect(const Channel &from, const Channel &to) {
            if (!isOutputPort(from.port) || !isInputPort(to.port)) {
                return std::unexpected(GraphError::InvalidDirection);
            }

            if (!nodeExists(from.nodeID) || !nodeExists(to.nodeID)) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            if (connexionExists(connexions_, from, to)) {
                return std::unexpected(ErrorType::ConnexionAlreadyExists);
            }

            if (pathExists(to.nodeID, from.nodeID)) {
                return std::unexpected(ErrorType::CycleDetected);
            }

            const bool occupied = std::ranges::any_of(
                connexions_,
                [&to](const Connexion &connexion) {
                    return connexion.to == to;
                }
            );

            if (occupied) {
                return std::unexpected(ErrorType::ChannelAlreadyConnected);
            }

            connexions_.emplace_back(from, to);
            return {};
        }

        [[nodiscard]] Result disconnect(const std::uint64_t inNodeId, const std::uint64_t outNodeId) {
            const auto edgeIt = std::ranges::find_if(
                connexions_,
                [inNodeId, outNodeId](const Connexion &edge) {
                    return edge.from.nodeID == inNodeId &&
                           edge.to.nodeID == outNodeId;
                }
            );

            if (edgeIt == connexions_.end()) {
                return std::unexpected(ErrorType::ConnexionNotFound);
            }

            connexions_.erase(edgeIt);
            return {};
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

        [[nodiscard]] std::expected<ExecutionPlan, ErrorType> compile() const {
            std::vector<std::size_t> layerIndices;

            auto sortedNodes = topologicalSort(layerIndices);

            if (!sortedNodes) {
                return std::unexpected(sortedNodes.error());
            }

            return ExecutionPlan {
                std::move(*sortedNodes),
                std::move(layerIndices)
            };
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
            std::vector<detail::PlanConnection> connections;
            connections.reserve(connexions_.size());
            for (const auto& connection : connexions_) {
                if (connection.from.channel > maxChannelIndex || connection.to.channel > maxChannelIndex) {
                    return std::unexpected(ErrorType::InvalidChannelIndex);
                }
                connections.push_back({
                    encode(static_cast<NodeID>(connection.from.nodeID), connection.from.port,
                           static_cast<ChannelIndex>(connection.from.channel)),
                    encode(static_cast<NodeID>(connection.to.nodeID), connection.to.port,
                           static_cast<ChannelIndex>(connection.to.channel))
                });
            }
            if (const auto result = plan->prepareBackend(config, connections, outputsNodes_); !result) {
                return std::unexpected(result.error());
            }
            return plan;
        }

    private:
        [[nodiscard]] bool nodeExists(const std::uint64_t id) const {
            const auto nodeIt = std::ranges::find(nodes_, id, &Node::id);
            return nodeIt != nodes_.end();
        }

        [[nodiscard]] bool pathExists(const std::uint64_t src, const std::uint64_t dst) const {
            if (src == dst)
                return true;

            std::vector<std::uint64_t> stack;
            std::unordered_set<std::uint64_t> visited;

            stack.push_back(src);

            while (!stack.empty()) {
                const std::uint64_t current = stack.back();
                stack.pop_back();

                if (!visited.insert(current).second)
                    continue;

                for (const auto &[from, to]: connexions_) {
                    if (from.nodeID != current)
                        continue;

                    if (to.nodeID == dst)
                        return true;

                    stack.push_back(to.nodeID);
                }
            }

            return false;
        }

        /**
         * @brief Trie les nœuds par couches topologiques.
         *
         * @param[out] layerIndices Limites des couches dans le vecteur retourné.
         *             Modifié uniquement en cas de succès.
         *
         * @return Les pointeurs vers les nœuds triés, ou une erreur.
         *
         * @note Un graphe vide produit un vecteur vide et les indices {0}.
         * @note Alloue de la mémoire : à appeler hors du thread audio.
         * @pre Le DAG ne doit pas être modifié pendant le tri.
         */
        [[nodiscard]]
        std::expected<std::vector<Node *>, ErrorType> topologicalSort(
            std::vector<std::size_t> &layerIndices
        ) const {
            const auto count = nodes_.size();

            // Associer chaque ID à sa position dans nodes_.
            std::unordered_map<std::uint64_t, std::size_t> indices;
            indices.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                indices.emplace(nodes_[i]->id(), i);
            }

            // Construire les dépendances dans le sens du signal : Out → In/Aux.
            std::vector<std::vector<std::size_t> > successors(count);

            for (const auto &connexion: connexions_) {
                const auto source = indices.find(connexion.from.nodeID);
                const auto destination = indices.find(connexion.to.nodeID);

                if (source == indices.end() || destination == indices.end()) {
                    return std::unexpected(ErrorType::NodeNotFound);
                }

                successors[source->second].push_back(destination->second);
            }

            // Dédupliquer les dépendances et compter les prédécesseurs.
            std::vector<std::size_t> remaining(count, 0);

            for (auto &list: successors) {
                std::ranges::sort(list);

                const auto duplicates = std::ranges::unique(list);
                list.erase(duplicates.begin(), duplicates.end());

                for (const auto destination: list) {
                    ++remaining[destination];
                }
            }

            std::vector<std::size_t> ready;
            std::vector<std::size_t> next;

            ready.reserve(count);
            next.reserve(count);

            // Première couche : les nœuds sans prédécesseur.
            for (std::size_t i = 0; i < count; ++i) {
                if (remaining[i] == 0) {
                    ready.push_back(i);
                }
            }

            std::vector<Node *> result;
            result.reserve(count);

            std::vector<std::size_t> offsets{0};
            offsets.reserve(count + 1);

            while (!ready.empty()) {
                // Garantir un ordre reproductible dans chaque couche.
                std::ranges::sort(
                    ready,
                    [this](const std::size_t left, const std::size_t right) {
                        return nodes_[left]->id() < nodes_[right]->id();
                    }
                );

                next.clear();

                for (const auto index: ready) {
                    result.push_back(nodes_[index].get());

                    for (const auto successor: successors[index]) {
                        --remaining[successor];

                        if (remaining[successor] == 0) {
                            next.push_back(successor);
                        }
                    }
                }

                // Marquer la fin de la couche qui vient d'être ajoutée.
                offsets.push_back(result.size());

                ready.swap(next);
            }

            if (result.size() != count) {
                return std::unexpected(ErrorType::CycleDetected);
            }

            layerIndices = std::move(offsets);
            return result;
        }

        // Sole control-owned storage; only unique_ptr handles move, never Nodes.
        std::vector<std::unique_ptr<Node> > nodes_;
        // Removed Nodes remain owned until every plan borrow retires on control.
        std::vector<std::unique_ptr<Node>> retiredNodes_;
        std::vector<Connexion> connexions_;
        std::vector<NodeID> outputsNodes_;
        detail::NodeIDSequence identities_; ///< Committed IDs never become available again.
        std::size_t maxOwnedNodes_ = maxNodeID; ///< Live + retired quota; immutable after construction.
    };
}
