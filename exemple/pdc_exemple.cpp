// Deux chemins : Source -> Mix.In et Source -> Retard64 -> Mix.Aux -> Sortie.
// Le compilateur retarde automatiquement le chemin direct de 64 samples.
#include <array>
#include <cstddef>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;

using namespace mka::audio::dsp;

namespace {
    class Retard64 final : public Node {
    public:
        explicit Retard64(NodeID id) : Node(id, {.layoutInput = {0}, .layoutOutput = {0}}) {}
        LatencySamples latencySamples() const noexcept override { return 64; }
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f) {
                ctx.outputs[0].buffer[f] = history_[position_];
                history_[position_] = ctx.inputs[0].buffer[f];
                position_ = (position_ + 1) % history_.size();
            }
        }
    private:
        std::array<float, 64> history_{}; // État DSP conservé entre les blocs.
        std::size_t position_ = 0;
    };

    class Mix final : public Node {
    public:
        explicit Mix(NodeID id)
            : Node(id, {.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f)
                ctx.outputs[0].buffer[f] = ctx.inputs[0].buffer[f] + ctx.auxiliary[0].buffer[f];
        }
    };
}

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

    // latencySamples() décrit le retard réel du DSP. compile() prépare le PDC
    // et ses historiques hors audio ; ils commencent à zéro.
    auto plan = graph.compile(BackendConfig{.maxFrames = 128, .inputChannels = 1, .outputChannels = 1});
    if (!plan) return 1;

    std::array<float, 128> input{};
    input[0] = 1.F; // Une impulsion envoyée dans les deux branches.
    std::array<float, 128> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};
    plan->exec(mka::audio::AudioProcessContext{inputs, outputs, 128});

    // Affichage hors callback : les deux branches se rejoignent au sample 64.
    std::cout << "Signal mélangé au sample 64 : " << samples[64] << '\n';
}
