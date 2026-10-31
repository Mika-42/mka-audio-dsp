//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>
export module mka.audio.dsp.node;
import mka.audio.dsp.channel;
import mka.audio.dsp.process_context;

export namespace mka::audio::dsp {

    class Node {
    public:

        enum class Layout : std::uint8_t {
            Mono,
            Stereo,
            Surround_3_0,
            Surround_3_1,
            Surround_4_0,
            Surround_5_1,
            Surround_7_1,
            Custom
        };

        struct Config {
            Layout layoutInput;
            Layout layoutOutput;
        };

        explicit Node(const std::uint32_t id, const Config &cfg) noexcept : id_(id), config_(cfg) {}

        [[nodiscard]] Channel channel(const PortType port, const std::uint64_t channel) const noexcept {
            return { .nodeID = id_, .channel = channel, .port = port };
        };

        [[nodiscard]] const std::uint64_t& id() const noexcept {
            return id_;
        }

        virtual void process(const ProcessContext& context) noexcept = 0;

        virtual ~Node() = default;

    private:
        std::uint64_t id_;
        Config config_;
    };
}
