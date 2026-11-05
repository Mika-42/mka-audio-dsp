/**
 * @file swap_exemple.cpp
 * @brief Callback-boundary publication and retirement of a replacement gain.
 * @author Mika
 */
// Replace gain 0.5 with gain 0.25 at the next callback boundary.
#include <array>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;
import mka.audio.dsp.plan_switcher;

using namespace mka::audio::dsp;

/// @brief Publish a replacement gain and retire the old plan after the next callback.
int main() {
    // The DAG must outlive all plans borrowing its Nodes.
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
    if (!player.initialize(graph, config)) return 1; // Control, before starting the backend.

    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};
    const mka::audio::AudioProcessContext block{inputs, outputs, 4};

    player.exec(block); // Audio: A processes this block.
    std::cout << "Premier sample avec A : " << samples[0] << '\n'; // Outside audio processing.

    // Control: prepare a distinct instance without mutating the one used by A.
    const auto gainB = graph.add<example::Gain>(0.25F);
    if (!gainB) return 1;
    if (!graph.remove(*gainA)) return 1; // Also removes its connections from the desired graph.
    if (!graph.connect(encode(*gainB, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*gainB, PortType::Out, 0))) return 1;
    if (!player.prepareAndPublish(graph, config)) return 1;

    player.exec(block); // Audio: adopt B at the boundary, then process the block.
    std::cout << "Premier sample avec B : " << samples[0] << '\n';

    // Control: ACK now permits releasing A and its previous gain.
    if (!player.retireAcknowledgedPlan()) return 1;
    (void)graph.collectRetiredNodes();

    // Here exec() is synchronous. With a real backend, stop and join its
    // audio thread before this call; no further callback may enter.
    if (!player.shutdownAfterBackendStopped()) return 1;
}
