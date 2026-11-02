// Exemple minimal : une entrée mono -> gain 0.5 -> une sortie physique.
#include <array>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;

using namespace mka::audio::dsp;

int main() {
    // CONTRÔLE : créer les processeurs. Le DAG en reste propriétaire.
    DAG graph;
    const auto source = graph.add<example::Source>();
    const auto gain = graph.add<example::Gain>(0.5F);
    const auto output = graph.add<example::Output>();
    if (!source || !gain || !output) return 1;

    // connect(destination, source) relie les canaux LOGIQUES des Nodes.
    if (!graph.connect(encode(*gain, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*gain, PortType::Out, 0))) return 1;
    if (!graph.markAsOutput(*output)) return 1;

    // Préparer l'ordre et les buffers avant de commencer le traitement audio.
    auto plan = graph.compile(BackendConfig{.maxFrames = 4, .inputChannels = 1, .outputChannels = 1});
    if (!plan) return 1;

    // Dans une application, ces buffers seraient fournis par le backend audio.
    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};

    // AUDIO : appeler exec() pour chaque bloc, avec les capacités préparées.
    plan->exec(mka::audio::AudioProcessContext{inputs, outputs, 4});

    // Afficher le résultat après le traitement, hors callback.
    for (float sample : samples) std::cout << sample << ' ';
    std::cout << '\n';
    // À la sortie : le plan est libéré avant le DAG, après le dernier callback.
}
