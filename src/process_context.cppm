module;

#include <cstddef>
#include <concepts>
#include <functional>
#include <span>
#include <type_traits>

export module mka.audio.dsp.process_context;
export import mka.audio.dsp.channel;

export namespace mka::audio::dsp {

    /**
     * @brief Borrowed logical and physical channel views for one process call.
     * @details Descriptors cannot be rebound through these const spans, while
     * OutputChannel samples remain writable. Logical outputs retain plan-owned
     * storage even when the node also sees separate physical backend outputs.
     * For sources, inputs exposes all current backend inputs in physical order;
     * otherwise it exposes prepared logical inputs. Auxiliary stays logical.
     * Backend output views are empty for nodes without the output role.
     * @pre Descriptor storage and at least frames samples per consumed channel
     * remain valid until process returns. Inputs are read-only. Backend pointers
     * belong to the backend for this callback only and must never be retained.
     * Internal/backend destinations must not accidentally alias. No sample may
     * be accessed for frames == 0. This aggregate owns and allocates nothing.
     */
    struct ProcessContext {
        std::span<const InputChannel> inputs;
        std::span<const InputChannel> auxiliary;
        std::span<const OutputChannel> outputs;
        std::span<const OutputChannel> backendOutputs;
        FrameCount frames = 0;
    };

    /**
     * @brief Internal views and limits prepared on control before processing.
     * @details Spans borrow stable internal descriptor arrays and plan buffers;
     * they must never hold callback-only backend views. Roles are supplied by
     * the caller/compiler, not inferred from layouts or detected from a DAG.
     * Source and output roles are independent and do not suppress successors.
     * Physical capacities are counts, not ChannelID/ChannelIndex fields.
     * @pre Internal samples cover maxFrames; all referenced internal storage
     * and these bindings stay stable while processing. Pointer-presence checks
     * cannot prove sample extents, lifetime or aliasing safety.
     */
    struct PreparedContextBindings {
        std::span<const InputChannel> inputs;
        std::span<const InputChannel> auxiliary;
        std::span<const OutputChannel> outputs;
        FrameCount maxFrames = 0;
        std::size_t maxBackendInputChannels = 0;
        std::size_t maxBackendOutputChannels = 0;
        bool isSource = false;
        bool isOutput = false;
    };

    /**
     * @brief Preflight prepared internal pointer presence on the control thread.
     * @return NullBuffer for a selected null pointer with maxFrames > 0,
     * otherwise success. Source inputs are supplied by the callback instead.
     * @note No sample reads, allocation, I/O or locks. Actual buffer extents and
     * backend capacities must be guaranteed by the owner during preparation.
     */
    [[nodiscard]] Result validateContextBindings(const PreparedContextBindings& prepared) noexcept {
        if (!prepared.isSource) {
            if (const auto result = validateChannelBuffers(prepared.inputs, prepared.maxFrames); !result) {
                return result;
            }
        }
        if (const auto result = validateChannelBuffers(prepared.auxiliary, prepared.maxFrames); !result) {
            return result;
        }
        return validateChannelBuffers(prepared.outputs, prepared.maxFrames);
    }

    /**
     * @brief Bind callback-local views and synchronously consume one context.
     * @param prepared Stable internal views, roles and prevalidated capacities.
     * @param backendInputs All current physical inputs, in backend order.
     * @param backendOutputs All current physical outputs, in backend order.
     * @param frames Actual samples per channel in this callback.
     * @param consume void(const ProcessContext&) noexcept consumer, e.g. a call
     * to Node::process. It must not allocate, lock, log, wait or retain pointers.
     * @return False on frame/count overflow or a selected null buffer, without
     * invoking consume or accessing samples. True means accepted; zero frames
     * is a no-op and does not invoke consume either.
     * @details Guard counts against prepared capacities before exposing views.
     * These bounded callback guards do not prepare/reallocate storage in RT.
     * Every call uses its current backend spans; none are stored in bindings or
     * persistent state. The local context is valid only during consume.
     * @pre All supplied spans reference valid descriptors for the call. Consumed
     * samples cover frames and internal buffers cover prepared.maxFrames.
     * Backend descriptors have channel == 0 (ignored); physical numbering is by
     * position. Capacity/lifetime/aliasing cannot be established from raw spans.
     * @note No allocation, sample clearing, locks or exception path is introduced.
     * Fail-safe rejection touches no samples; external output silencing belongs
     * to the callback owner, which knows the actual external buffer capacity.
     */
    template<typename Consumer>
        requires std::is_nothrow_invocable_v<Consumer&, const ProcessContext&> &&
                 std::same_as<std::invoke_result_t<Consumer&, const ProcessContext&>, void>
    [[nodiscard]] bool withProcessContext(
        const PreparedContextBindings& prepared,
        const std::span<const InputChannel> backendInputs,
        const std::span<const OutputChannel> backendOutputs,
        const FrameCount frames,
        Consumer&& consume) noexcept {
        if (frames > prepared.maxFrames ||
            (prepared.isSource && backendInputs.size() > prepared.maxBackendInputChannels) ||
            (prepared.isOutput && backendOutputs.size() > prepared.maxBackendOutputChannels)) {
            return false;
        }
        if (frames == 0) {
            return true;
        }

        const ProcessContext context{
            .inputs = prepared.isSource ? backendInputs : prepared.inputs,
            .auxiliary = prepared.auxiliary,
            .outputs = prepared.outputs,
            .backendOutputs = prepared.isOutput ? backendOutputs : std::span<const OutputChannel>{},
            .frames = frames
        };
        if (!validateChannelBuffers(context.inputs, frames) ||
            !validateChannelBuffers(context.auxiliary, frames) ||
            !validateChannelBuffers(context.outputs, frames) ||
            !validateChannelBuffers(context.backendOutputs, frames)) {
            return false;
        }
        std::invoke(consume, context);
        return true;
    }
}
