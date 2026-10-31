//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>
export module mka.audio.dsp.node;
export import mka.audio.dsp.config;
import mka.audio.dsp.channel;
import mka.audio.dsp.process_context;

export namespace mka::audio::dsp {

    class Node {
    public:

        /// Compatibility name for the public value type; no second Config type.
        using Config = mka::audio::dsp::Config;

        /**
         * @brief Copy an independently owned configuration on the control thread.
         * @pre validateConfig(cfg) succeeds. This constructor does not validate.
         * @note Copying vectors can allocate/throw; construction is not noexcept.
         * Keep this snapshot stable while a plan uses it. No runtime editing API
         * is provided; backend access is not determined by these layouts.
         */
        explicit Node(const std::uint32_t id, const Config &cfg) : id_(id), config_(cfg) {}

        /// Borrow this node's stable, read-only layout snapshot; no allocation.
        [[nodiscard]] const Config& config() const noexcept {
            return config_;
        }

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
