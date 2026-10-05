module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <unordered_set>
#include <vector>
export module mka.audio.dsp.graph;
export import mka.audio.dsp.error;

// non-RT graph
namespace mka::audio::dsp {
    export using NodeID = std::uint32_t;

    namespace {
        struct Node {
            std::string name;
            NodeID id;
        };

        struct Edge {
            NodeID src;
            NodeID dst;
        };
    }

    export class DAGraph {
    public:
        [[nodiscard]] NodeID addNode(const std::string &name) {
            nodes_.emplace_back(name, nextId);
            return nextId++;
        }

        [[nodiscard]] Result removeNode(const NodeID id) {
            const auto nodeIt =
                    std::ranges::find(nodes_, id, &Node::id);

            if (nodeIt == nodes_.end()) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            nodes_.erase(nodeIt);

            std::erase_if(edges_, [id](const Edge &edge) {
                return edge.src == id || edge.dst == id;
            });

            return {};
        }

        [[nodiscard]] Result connect(const NodeID src, const NodeID dst) {

            if (!nodeExists(src) || !nodeExists(dst)) {
                return std::unexpected(ErrorType::NodeNotFound);
            }

            if (edgeExists(src, dst)) {
                return std::unexpected(ErrorType::EdgeAlreadyExists);
            }

            if (pathExists(dst, src)) {
                return std::unexpected(ErrorType::CycleDetected);
            }

            edges_.emplace_back(src, dst);
            return {};
        }

        [[nodiscard]] Result disconnect(const NodeID src, const NodeID dst) {
            const auto edgeIt = std::ranges::find_if(
                edges_,
                [src, dst](const Edge &edge) {
                    return edge.src == src &&
                           edge.dst == dst;
                }
            );

            if (edgeIt == edges_.end()) {
                return std::unexpected(ErrorType::EdgeNotFound);
            }

            edges_.erase(edgeIt);
            return {};
        }

    private:
        [[nodiscard]] bool nodeExists(const NodeID id) const {
            const auto nodeIt = std::ranges::find(nodes_, id, &Node::id);
            return nodeIt != nodes_.end();
        }

        [[nodiscard]] bool edgeExists(const NodeID src, const NodeID dst) const {
            const auto edgeIt = std::ranges::find_if(
                edges_,
                [src, dst](const Edge &edge) {
                    return edge.src == src &&
                           edge.dst == dst;
                }
            );

            return edgeIt != edges_.end();
        }

        [[nodiscard]] bool pathExists(const NodeID src, const NodeID dst) const
        {
            if (src == dst)
                return true;

            std::vector<NodeID> stack;
            std::unordered_set<NodeID> visited;

            stack.push_back(src);

            while (!stack.empty()) {
                const NodeID current = stack.back();
                stack.pop_back();

                if (!visited.insert(current).second)
                    continue;

                for (const Edge& edge : edges_) {
                    if (edge.src != current)
                        continue;

                    if (edge.dst == dst)
                        return true;

                    stack.push_back(edge.dst);
                }
            }

            return false;
        }

        std::vector<Node> nodes_;
        std::vector<Edge> edges_;
        inline static NodeID nextId = 0;
    };
}
