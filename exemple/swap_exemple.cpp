// Remplacer le gain 0.5 par un gain 0.25, au prochain callback.
#include <array>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;
import mka.audio.dsp.plan_switcher;

using namespace mka::audio::dsp;

int main() {
    // Le DAG doit survivre aux plans qui empruntent ses Nodes.
    DAG graph;
    const auto source = graph.add<example::Source>();
    const auto gainA = graph.add<example::Gain>(0.5F);
    const auto output = graph.add<example::Output>();
    if (!source || !gainA || !output) return 1;
    if (!graph.connect(encode(*gainA, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*gainA, PortType::Out, 0))) return 1;
    if (!graph.markAsOutput(*output)) return 1;

    const BackendConfig config{.maxFrames = 4, .inputChannels = 1, .outputChannels = 1};
    PlanSwitcher player;
    if (!player.initialize(graph, config)) return 1; // Contrôle, avant le backend.

    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};
    const mka::audio::AudioProcessContext block{inputs, outputs, 4};

    player.exec(block); // Audio : A traite ce bloc.
    std::cout << "Premier sample avec A : " << samples[0] << '\n'; // Hors audio.

    // Contrôle : préparer une autre instance, sans modifier celle utilisée par A.
    const auto gainB = graph.add<example::Gain>(0.25F);
    if (!gainB) return 1;
    if (!graph.remove(*gainA)) return 1; // Retire aussi ses connexions du DAG désiré.
    if (!graph.connect(encode(*gainB, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*gainB, PortType::Out, 0))) return 1;
    if (!player.prepareAndPublish(graph, config)) return 1;

    player.exec(block); // Audio : adopte B à la frontière, puis traite le bloc.
    std::cout << "Premier sample avec B : " << samples[0] << '\n';

    // Contrôle : l'ACK permet maintenant de libérer A et son ancien gain.
    if (!player.retireAcknowledgedPlan()) return 1;
    (void)graph.collectRetiredNodes();

    // Ici exec() est synchrone. Avec un vrai backend, arrêter et joindre son
    // thread audio avant cet appel : aucun callback ne doit encore pouvoir entrer.
    if (!player.shutdownAfterBackendStopped()) return 1;
}
