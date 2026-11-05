/**
 * @file nodes.hpp
 * @brief Small mono processors shared by the usage examples.
 * @author Mika
 */
#pragma once
#include <algorithm>
#include <vector>

import mka.audio.dsp.node;

namespace example {
    using namespace mka::audio::dsp;

    // The examples share these three mono Nodes. Logical channel zero is
    // separate from the physical lane selected by the source and output.
    /// @brief Copies a physical input into a logical signal for downstream processors.
    class Source final : public Node {
    public:
        /// @brief Declare the source's logical outputs while leaving physical-input selection to
        /// processing.
        explicit Source(NodeID id) : Node(id, {.layoutOutput = {0}}) {}
        /// @brief Copy the first physical input into the logical mono output.
        void process(const ProcessContext& ctx) noexcept override {
            // A topological source receives read-only physical inputs.
            std::copy_n(ctx.inputs[0].buffer, ctx.frames, ctx.outputs[0].buffer);
        }
    };

    /// @brief Applies a fixed gain to its logical input signal.
    class Gain final : public Node {
    public:
        /// @brief Declare the logical ports and store the immutable gain multiplier.
        Gain(NodeID id, float gain)
            : Node(id, {.layoutInput = {0}, .layoutOutput = {0}}), gain_(gain) {}
        /// @brief Multiply the logical input by the fixed gain without allocating.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f)
                ctx.outputs[0].buffer[f] = gain_ * ctx.inputs[0].buffer[f];
        }
    private:
        /// @brief Fixed multiplication factor applied by the processor.
        const float gain_; // Do not mutate DSP state borrowed by the active plan.
    };

    /// @brief Copies a logical signal into the selected physical output.
    class Output final : public Node {
    public:
        /// @brief Declare the logical input consumed by this physical-output processor.
        explicit Output(NodeID id) : Node(id, {.layoutInput = {0}}) {}
        /// @brief Copy the logical mono input into physical output zero.
        void process(const ProcessContext& ctx) noexcept override {
            // markAsOutput() provides access to all physical outputs.
            std::copy_n(ctx.inputs[0].buffer, ctx.frames, ctx.backendOutputs[0].buffer);
        }
    };
}
