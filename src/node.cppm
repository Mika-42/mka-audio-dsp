//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>
#include <memory>
#include <utility>
export module mka.audio.dsp.node;
export import mka.audio.dsp.config;
export import mka.audio.dsp.channel;
export import mka.audio.dsp.process_context;

// Defined in this module to avoid attaching cross-module DAG/ExecutionPlan
// forward declarations to the Node module. Tokens borrow; they do not own Node.
export namespace mka::audio::dsp::detail {
    struct NodeBorrowState {};
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
         * @brief Intrinsic latency in whole samples, zero unless overridden.
         * @note Read on control from coherent state. Preparing a new plan must
         * not mutate DSP state simultaneously used by the active plan.
         */
        [[nodiscard]] virtual std::uint32_t latencySamples() const noexcept {
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
            return node.borrowState_;
        }
        static bool inUse(const Node& node) noexcept {
            return node.borrowState_.use_count() > 1;
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
