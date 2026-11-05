/**
 * @file pdc_exemple.cpp
 * @brief Direct and delayed mono branches with automatic latency compensation.
 * @author Mika
 */
// Two paths: Source -> Mix.In and Source -> Retard64 -> Mix.Aux -> Output.
// The compiler automatically delays the direct path by 64 samples.
#include <array>
#include <cstddef>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;

using namespace mka::audio::dsp;

namespace {
    /// @brief Produces an actual 64-sample delay and declares that latency to the compiler.
    class Retard64 final : public Node {
    public:
        /// @brief Declare one main input and output with an initially zero 64-sample history.
        explicit Retard64(NodeID id) : Node(id, {.layoutInput = {0}, .layoutOutput = {0}}) {}
        /// @brief Return the fixed algorithmic delay declared by this fixture processor.
        LatencySamples latencySamples() const noexcept override { return 64; }
        /// @brief Process the mono signal through the persistent 64-sample circular history.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f) {
                ctx.outputs[0].buffer[f] = history_[position_];
                history_[position_] = ctx.inputs[0].buffer[f];
                position_ = (position_ + 1) % history_.size();
            }
        }
    private:
        /// @brief Owned persistent delay samples, initialized to zero during preparation.
        std::array<float, 64> history_{}; // DSP state persists across blocks.
        /// @brief Cursor into persistent DSP history; empty blocks do not advance it.
        std::size_t position_ = 0;
    };

    /// @brief Adds main and auxiliary signals into one logical output.
    class Mix final : public Node {
    public:
        /// @brief Declare the main, auxiliary and logical output channels of the junction.
        explicit Mix(NodeID id)
            : Node(id, {.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}) {}
        /// @brief Sum main and auxiliary mono signals into the logical output.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f)
                ctx.outputs[0].buffer[f] = ctx.inputs[0].buffer[f] + ctx.auxiliary[0].buffer[f];
        }
    };
}

/// @brief Join direct and delayed mono branches with automatically prepared compensation.
int main() {
    DAG graph;
    const auto source = graph.add<example::Source>();
    const auto delay = graph.add<Retard64>();
    const auto mix = graph.add<Mix>();
    const auto output = graph.add<example::Output>();
    if (!source || !delay || !mix || !output) return 1;

    if (!graph.connect(encode(*delay, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*mix, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*mix, PortType::Aux, 0), encode(*delay, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*mix, PortType::Out, 0))) return 1;
    if (!graph.markAsOutput(*output)) return 1;

    // latencySamples() declares the actual DSP delay. compile() prepares PDC
    // and its initially zero histories outside audio processing.
    auto plan = graph.compile(BackendConfig{.maxFrames = 128, .inputChannels = 1, .outputChannels = 1});
    if (!plan) return 1;

    std::array<float, 128> input{};
    input[0] = 1.F; // Send one impulse into both branches.
    std::array<float, 128> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};
    plan->exec(mka::audio::AudioProcessContext{inputs, outputs, 128});

    // Print outside the callback: both branches arrive together at sample 64.
    std::cout << "Signal mélangé au sample 64 : " << samples[64] << '\n';
}
