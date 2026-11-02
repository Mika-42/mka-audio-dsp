#pragma once
#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <vector>

// Include after importing mka.audio.dsp.dag. No heap or retained context in process.
namespace pool_test {
    using namespace mka::audio::dsp;

    class SumNode final : public Node {
    public:
        explicit SumNode(NodeID id, Config cfg = {{0,1},{0,1},{0,1}}) : Node(id, cfg) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f) {
                float sample = static_cast<float>(id());
                for (const auto& input : ctx.inputs) sample += input.buffer[f] * 0.5F;
                for (const auto& input : ctx.auxiliary) sample += input.buffer[f] * 0.25F;
                for (std::size_t c = 0; c < ctx.outputs.size(); ++c) {
                    ctx.outputs[c].buffer[f] = sample + static_cast<float>(c);
                    if (capture[c]) capture[c][f] = sample + static_cast<float>(c);
                }
                for (std::size_t c = 0; c < ctx.backendOutputs.size(); ++c)
                    ctx.backendOutputs[c].buffer[f] = sample + static_cast<float>(c);
            }
        }
        // Optional preallocated test trace; copying samples performs no allocation.
        std::array<float*,2> capture{};
    };

    struct Pipeline {
        DAG graph;
        std::vector<NodeID> ids;

        NodeID add() {
            const auto id = graph.add<SumNode>();
            if (!id) throw std::bad_alloc{};
            ids.push_back(*id);
            return *id;
        }
        Connection edge(NodeID from, NodeID to, PortType port = PortType::In,
                        ChannelIndex destination = 0, ChannelIndex source = 0) {
            const Connection connection{encode(from, PortType::Out, source), encode(to, port, destination)};
            if (!graph.connect(connection.to, connection.from)) throw std::logic_error("invalid test edge");
            return connection;
        }
    };
}
