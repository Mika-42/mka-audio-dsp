//
// Created by mika on 10/17/26.
//

#include <cstdint>

import mka.audio.dsp.dag;
import mka.audio.dsp.channel;

namespace {
    class StereoEffect final : public mka::audio::dsp::Node {
    public:
        explicit StereoEffect(const std::uint64_t id)
            : Node(id, {
                       .layoutInput = Layout::Stereo,
                       .layoutOutput = Layout::Stereo
                   }) {
        }
    };
}

int main() {
    mka::audio::dsp::DAG graph;

    const auto& source = graph.add<StereoEffect>();
    const auto& effect = graph.add<StereoEffect>();

    const auto result = graph.connect(
        effect.channel(mka::audio::dsp::PortType::In, 0),
        source.channel(mka::audio::dsp::PortType::Out, 0)
    );

    if (!result) {
        // Traiter l'erreur.
    }

    const auto effectId = effect.id();
}
