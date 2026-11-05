/**
 * @file delay_task.cppm
 * @brief Prepared edge-compensation operations and sample-range validation.
 * @author Mika
 */
module;
#include <cstdint>
#include <expected>
#include <limits>

export module mka.audio.dsp.delay_task;
export import mka.audio.dsp.delay_line;

export namespace mka::audio::dsp::detail {
    /**
     * @brief Validate two internal sample ranges on control before publishing.
     * @details Checks nulls, representable byte endpoints and any overlap,
     * including partial overlap. No pointer arithmetic/dereference on invalid
     * views. Empty capacity permits null views, as no sample can be accessed.
     * Actual allocation ownership/extent is proved separately by the plan pool.
     */
    [[nodiscard]] Result validateDelayBinding(const float* source,float* destination,
                                             const FrameCount capacity) noexcept {
        if (capacity==0) return {};
        if (!source || !destination) return std::unexpected(Error{ErrorCode::InvalidParameter});
        const auto bytes=checkedSizeMultiply(capacity,sizeof(float));
        if (!bytes) return std::unexpected(bytes.error());
        const auto from=reinterpret_cast<std::uintptr_t>(source);
        const auto to=reinterpret_cast<std::uintptr_t>(destination);
        constexpr auto maximum=std::numeric_limits<std::uintptr_t>::max();
        if (*bytes>maximum || from>maximum-*bytes || to>maximum-*bytes)
            return std::unexpected(Error{ErrorCode::ArithmeticOverflow});
        if (from<to+*bytes && to<from+*bytes)
            return std::unexpected(Error{ErrorCode::InvalidParameter});
        return {};
    }

    /**
     * @brief Concrete internal PDC operation; no Node, virtual dispatch or backend view.
     * @details The plan exclusively owns line; source/destination borrow its
     * stable, disjoint pool slots. Prepare and bind on serialized control, before
     * publication. Copying duplicates history but borrows the same views until
     * rebound; ExecutionPlan always rebinds after a pool copy. Moves/copies,
     * preparation and destruction require quiescence. Never resize or rebind RT.
     */
    struct DelayTask {
        /// @brief Read-only producer samples in a stable pool slot.
        const float* source=nullptr;
        /// @brief Borrowed writable sample buffer for the delayed signal.
        float* destination=nullptr;
        /// @brief Positive whole-sample compensation applied by this operation.
        std::uint64_t delaySamples=0;
        /// @brief Persistent history owned by this compensation operation.
        DelayLine line;

        /// D=0 belongs to a direct binding, never a materialized DelayTask.
        [[nodiscard]] Result prepare(const std::uint64_t delay,const DelayLineLimits& limits={}) {
            if (delay==0) return std::unexpected(Error{ErrorCode::InvalidParameter});
            if (const auto result=line.prepare(delay,limits); !result) return result;
            delaySamples=delay;
            return {};
        }

        /// Validate first, then install views. Failure preserves previous views and DSP state.
        [[nodiscard]] Result bind(const float* input,float* output,const FrameCount capacity) noexcept {
            if (delaySamples==0 || line.delaySamples()!=delaySamples)
                return std::unexpected(Error{ErrorCode::InvalidParameter});
            if (const auto result=validateDelayBinding(input,output,capacity); !result) return result;
            source=input; destination=output;
            return {};
        }

        /**
         * @brief Run the prepared delay on exactly frames samples, without ownership work.
         * @pre Prepared, bound, active on one audio thread; frames fits the
         * capacity validated by bind. No lookup, allocation, lock or exception.
         */
        void execute(const FrameCount frames) noexcept { line.process(source,destination,frames); }
    };
}
