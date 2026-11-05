/**
 * @file pool_pipeline.hpp
 * @brief Test fixtures for pool pipeline.
 * @author Mika
 */
#pragma once
#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <vector>

// Include after importing mka.audio.dsp.dag. No heap or retained context in process.
namespace pool_test {
    using namespace mka::audio::dsp;

    /// @brief Adds its main and auxiliary signals into logical and selected physical outputs.
    class SumNode final : public Node {
    public:
        /// @brief Declare the main, auxiliary and logical output layouts of the summing processor.
        explicit SumNode(NodeID id, Config cfg = {{0,1},{0,1},{0,1}}) : Node(id, cfg) {}
        /// @brief Sum the main and auxiliary stereo paths and capture the resulting logical samples.
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
        /// @brief Borrowed destination sample pointers used to retain selected logical outputs.
        std::array<float*,2> capture{};
    };

    /// @brief Owns the processors and connections of a reusable audio test graph.
    struct Pipeline {
        /// @brief Owned desired DAG or detached graph values used by this fixture.
        DAG graph;
        /// @brief Stable identities of processors created for this fixture topology.
        std::vector<NodeID> ids;

        /// @brief Insert a fixture processor and retain its identity for later graph construction.
        NodeID add() {
            const auto id = graph.add<SumNode>();
            if (!id) throw std::bad_alloc{};
            ids.push_back(*id);
            return *id;
        }
        /// @brief Build the complete logical edge identity used by the fixture.
        Connection edge(NodeID from, NodeID to, PortType port = PortType::In,
                        ChannelIndex destination = 0, ChannelIndex source = 0) {
            const Connection connection{encode(from, PortType::Out, source), encode(to, port, destination)};
            if (!graph.connect(connection.to, connection.from)) throw std::logic_error("invalid test edge");
            return connection;
        }
    };
}
