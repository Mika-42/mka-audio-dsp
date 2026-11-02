//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
export module mka.audio.dsp.node;
export import mka.audio.dsp.config;
export import mka.audio.dsp.channel;
export import mka.audio.dsp.process_context;

// Defined in this module to avoid attaching cross-module DAG/ExecutionPlan
// forward declarations to the Node module. Tokens borrow; they do not own Node.
export namespace mka::audio::dsp::detail {
    // Only serialized control accesses these fields. Audio never reads them.
    struct NodeBorrowState {
        mutable bool hasLatency = false;
        mutable LatencySamples latency = 0;
        mutable std::uint32_t sampleRate = 0;
    };
    using NodeBorrowToken = std::shared_ptr<const NodeBorrowState>;
    struct NodeBorrowAccess;
    struct NodeConfigAccess;
}

export namespace mka::audio::dsp {

    /**
     * @brief Abstract processor owned by DAG and borrowed by prepared plans.
     * @details Identity/address and Config remain stable while any plan uses
     * this node. Construct, prepare, assign latency snapshots and destroy on
     * control only, after all processing and plan borrows have quiesced.
     * Backend pointers are borrowed from ProcessContext for one call and must
     * never be retained as Node state. No universal prepare() API is imposed.
     */
    class Node {
    public:

        /// Compatibility name for the public value type; no second Config type.
        using Config = mka::audio::dsp::Config;

        /**
         * @brief Copy an independently owned configuration on the control thread.
         * @pre id != invalidNodeID and validateConfig(cfg) succeeds. Identity is
         * assigned on control by DAG; this constructor does not validate it.
         * @note Copying vectors can allocate/throw; construction is not noexcept.
         * Keep this snapshot stable while a plan uses it. DAG may replace it on
         * control only after all plan borrows end; backend access is not
         * determined by these layouts.
         */
        explicit Node(const NodeID id, const Config &cfg) : id_(id), config_(cfg) {}

        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
        Node(Node&&) = delete;
        Node& operator=(Node&&) = delete;

        /**
         * @brief Borrow the current read-only layout snapshot, without allocation.
         * @details The Config object's address is stable. Its vector views may
         * be invalidated by a successful DAG::setConfig on an unborrowed Node;
         * no configuration contents change while any plan borrows the Node.
         */
        [[nodiscard]] const Config& config() const noexcept {
            return config_;
        }

        /**
         * @brief Numeric identity of a logical channel, independent of layouts.
         * @pre isValidPort(port). Validate decoded ports and wide raw indices
         * on control before calling; use toChannelIndex before narrowing.
         * @note Pure, allocation-free lookup: no graph or backend access.
         */
        [[nodiscard]] ChannelID channel(const PortType port, const ChannelIndex index) const noexcept {
            return encode(id_, port, index);
        }

        /// Stable logical identity returned by value, with no allocation.
        [[nodiscard]] NodeID id() const noexcept {
            return id_;
        }

        /**
         * @brief Process borrowed views synchronously on the audio thread.
         * @details Overrides must perform no allocation, deallocation, locks,
         * waits, logging, I/O or exception propagation. Read inputs, write only
         * permitted samples, and never retain context/backend pointers.
         */
        virtual void process(const ProcessContext& context) noexcept = 0;

        /**
         * @brief Algorithmic delay in whole samples at the effective sample rate.
         * @details Zero by default (for example a gain). A lookahead/plugin
         * declares the actual displacement of its output samples, common to
         * all audio paths/channels in V1. CPU duration, callback size and a
         * reverberation tail (TailInfo) are different quantities.
         * @note Capture on control for one generation; exec never queries this
         * method. Keep the declaration and corresponding DSP behavior stable
         * while any plan borrows this Node. A latency/rate change requires
         * quiescence of all such plans or a distinct, independently prepared
         * Node for candidate B. Never prepare/mutate a shared active Node to
         * obtain B's latency; atomically changing a number alone does not
         * coordinate a change of DSP behavior with publication.
         */
        [[nodiscard]] virtual LatencySamples latencySamples() const noexcept {
            return 0;
        }

        /// Public polymorphic destruction, on control after all borrows end.
        virtual ~Node() = default;

    private:
        friend struct detail::NodeBorrowAccess;
        friend struct detail::NodeConfigAccess;
        NodeID id_{};
        Config config_{};
        detail::NodeBorrowToken borrowState_ = std::make_shared<detail::NodeBorrowState>();
    };
}

export namespace mka::audio::dsp::detail {
    /** Internal control-side borrow tracking; never copied/released in process. */
    struct NodeBorrowAccess {
        static NodeBorrowToken retain(const Node& node) noexcept {
            // A new borrow epoch may observe a newly prepared DSP declaration.
            if (node.borrowState_.use_count() == 1) node.borrowState_->hasLatency = false;
            return node.borrowState_;
        }
        static bool inUse(const Node& node) noexcept {
            return node.borrowState_.use_count() > 1;
        }

        /** Caller owns at most one of the plan borrows counted here. */
        static std::expected<LatencySamples, Error> readLatency(
            const Node& node, const std::uint32_t sampleRate) noexcept {
            const auto& state = *node.borrowState_;
            if (state.hasLatency && node.borrowState_.use_count() > 1) {
                if (state.sampleRate != sampleRate)
                    return std::unexpected(Error{ErrorCode::NodeInUse, node.id()});
                return state.latency; // Never query mutable DSP shared with A.
            }
            if (node.borrowState_.use_count() > 2)
                return std::unexpected(Error{ErrorCode::NodeInUse, node.id()});
            return node.latencySamples(); // Exclusive/quiescent first preparation.
        }

        static void freezeLatency(const Node& node, const std::uint32_t sampleRate,
                                  const LatencySamples latency) noexcept {
            node.borrowState_->latency = latency;
            node.borrowState_->sampleRate = sampleRate;
            node.borrowState_->hasLatency = true;
        }
    };

    /**
     * @brief Internal control-side installation of an independently owned Config.
     * @pre The replacement is valid, no plan borrows node, and all external uses
     * of its old layout views have ceased. DAG::setConfig enforces the plan guard
     * and prepares the candidate before this non-allocating move.
     * @details No identity/address, DSP state, graph edge or output role changes.
     * Old vector storage is released on control. Not a public Node editing API.
     */
    struct NodeConfigAccess {
        static void replaceConfig(Node& node, Config replacement) noexcept {
            node.config_ = std::move(replacement);
        }
    };
}
