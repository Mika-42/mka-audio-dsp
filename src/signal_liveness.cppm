module;
#include <algorithm>
#include <cstddef>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

export module mka.audio.dsp.signal_liveness;
export import mka.audio.dsp.buffer_pool;

export namespace mka::audio::dsp {
    /// Dense control-side production identity; not a public ChannelID or history ID.
    using SignalID = std::size_t;
    enum class OperationKind { Node, Delay, Readback };

    /** Final serial operation after delay insertion; lists every actual read/write. */
    struct SignalOperation {
        OperationKind kind = OperationKind::Node;
        std::vector<SignalID> reads;
        std::vector<SignalID> writes;
    };

    /// Inclusive interval; a production with no reader expires after its producer.
    struct SignalLifetime {
        std::size_t producedAt = 0;
        std::size_t lastUse = 0;
        bool operator==(const SignalLifetime&) const noexcept = default;
    };

    /**
     * @brief Compute exact lifetimes on the final Node/Delay/readback sequence.
     * @details Each dense ID has exactly one producer. Readers (In, Aux, fan-out,
     * delay inputs, terminal exports) must follow that producer. A readback is an
     * explicit consumption after processing, not an assumed backend reservation.
     * Persistent delay histories and silence have no SignalID. Control only;
     * nothing in this temporary table is inspected or changed in audio.
     */
    [[nodiscard]] std::expected<std::vector<SignalLifetime>, CompileError>
    calculateLifetimes(const std::span<const SignalOperation> operations,
                       const std::size_t signalCount) try {
        if (signalCount > std::vector<SignalLifetime>{}.max_size())
            return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
        constexpr auto absent = std::numeric_limits<std::size_t>::max();
        std::vector<SignalLifetime> result(signalCount, {absent, absent});
        for (std::size_t i = 0; i < operations.size(); ++i) {
            for (const auto id : operations[i].reads) {
                if (id >= signalCount || result[id].producedAt == absent)
                    return std::unexpected(CompileError{ErrorCode::InvalidParameter});
                result[id].lastUse = i;
            }
            for (const auto id : operations[i].writes) {
                if (id >= signalCount || result[id].producedAt != absent)
                    return std::unexpected(CompileError{ErrorCode::InvalidParameter});
                result[id] = {i, i};
            }
        }
        if (std::ranges::any_of(result, [](auto life) { return life.producedAt == absent; }))
            return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(CompileError{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
    }
}
