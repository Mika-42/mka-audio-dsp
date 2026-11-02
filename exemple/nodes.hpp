#pragma once
#include <algorithm>
#include <vector>

import mka.audio.dsp.node;

namespace example {
    using namespace mka::audio::dsp;

    // Ces trois Nodes mono servent aux exemples. Le canal logique 0 est
    // distinct de la voie physique, choisie par la source et la sortie.
    class Source final : public Node {
    public:
        explicit Source(NodeID id) : Node(id, {.layoutOutput = {0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            // Une source topologique reçoit les entrées physiques en const.
            std::copy_n(ctx.inputs[0].buffer, ctx.frames, ctx.outputs[0].buffer);
        }
    };

    class Gain final : public Node {
    public:
        Gain(NodeID id, float gain)
            : Node(id, {.layoutInput = {0}, .layoutOutput = {0}}), gain_(gain) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f = 0; f < ctx.frames; ++f)
                ctx.outputs[0].buffer[f] = gain_ * ctx.inputs[0].buffer[f];
        }
    private:
        const float gain_; // Ne pas modifier un DSP emprunté par le plan actif.
    };

    class Output final : public Node {
    public:
        explicit Output(NodeID id) : Node(id, {.layoutInput = {0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            // markAsOutput() donne accès à toutes les sorties physiques.
            std::copy_n(ctx.inputs[0].buffer, ctx.frames, ctx.backendOutputs[0].buffer);
        }
    };
}
