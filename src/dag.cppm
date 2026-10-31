module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
export module mka.audio.dsp.dag;
export import mka.audio.dsp.error;
export import mka.audio.dsp.node;
import mka.audio.dsp.channel;
export import mka.audio.dsp.execution_plan;

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

    export class DAG {
    public:
        DAG() = default;
        DAG(const DAG&) = delete;
        DAG& operator=(const DAG&) = delete;
        // Replacing/transferring an owner could bypass remove()'s borrow guard.
        DAG(DAG&&) = delete;
        DAG& operator=(DAG&&) = delete;

        template<std::derived_from<Node> T>
        [[nodiscard]]
        T &add() {
            auto node = std::make_unique<T>(nextId);
            auto &reference = *node;

            nodes_.push_back(std::move(node));
            ++nextId;

            return reference;
        }

        [[nodiscard]] Result remove(const std::uint64_t nodeId) {
            const auto nodeIt = std::ranges::find(nodes_, nodeId, &Node::id);

            if (nodeIt == nodes_.end()) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            // Plans borrow Nodes without owning them. Defer this removal until
            // every plan has been retired on control; reject without mutation.
            if (detail::NodeBorrowAccess::inUse(**nodeIt)) {
                return std::unexpected(ErrorType::NodeInUse);
            }

            nodes_.erase(nodeIt);

            std::erase_if(connexions_, [nodeId](const Connexion &conn) {
                return conn.from.nodeID == nodeId || conn.to.nodeID == nodeId;
            });
            std::erase(outputsNodes_, static_cast<NodeID>(nodeId));

            return {};
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

        [[nodiscard]] const Node *node(const std::uint64_t id) const noexcept {
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

        std::vector<std::unique_ptr<Node> > nodes_;
        std::vector<Connexion> connexions_;
        std::vector<NodeID> outputsNodes_;
        std::uint64_t nextId = 1;
    };
}
