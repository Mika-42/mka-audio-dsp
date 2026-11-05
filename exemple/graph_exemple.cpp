/**
 * @file graph_exemple.cpp
 * @brief Minimal mono graph construction and one-block processing example.
 * @author Mika
 */
// Minimal example: mono input -> gain 0.5 -> physical output.
#include <array>
#include <expected>
#include <iostream>
#include <span>
#include "nodes.hpp"

import mka.audio.dsp.dag;

using namespace mka::audio::dsp;

/// @brief Build a mono source/gain/output graph and process one borrowed audio block.
int main() {
    // CONTROL: create the processors; the DAG retains ownership.
    DAG graph;
    const auto source = graph.add<example::Source>();
    const auto gain = graph.add<example::Gain>(0.5F);
    const auto output = graph.add<example::Output>();
    if (!source || !gain || !output) return 1;

    // connect(destination, source) connects the Nodes' LOGICAL channels.
    if (!graph.connect(encode(*gain, PortType::In, 0), encode(*source, PortType::Out, 0))) return 1;
    if (!graph.connect(encode(*output, PortType::In, 0), encode(*gain, PortType::Out, 0))) return 1;
    if (!graph.markAsOutput(*output)) return 1;

    // Prepare the processing order and buffers before starting audio.
    auto plan = graph.compile(BackendConfig{.maxFrames = 4, .inputChannels = 1, .outputChannels = 1});
    if (!plan) return 1;

    // An application receives these buffers from its audio backend.
    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> samples{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{samples.data()};

    // AUDIO: call exec() for each block within the prepared capacities.
    plan->exec(mka::audio::AudioProcessContext{inputs, outputs, 4});

    // Print the result after processing, outside the callback.
    for (float sample : samples) std::cout << sample << ' ';
    std::cout << '\n';
    // On exit, release the plan before the DAG, after the final callback.
}
