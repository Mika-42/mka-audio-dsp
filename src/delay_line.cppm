module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

export module mka.audio.dsp.delay_line;
export import mka.audio.dsp.memory_budget;

export namespace mka::audio::dsp {
    /// Inclusive control-side limits for one history's requested float payload.
    /// The containing plan separately accounts for its aggregate retained memory.
    struct DelayLineLimits {
        std::uint64_t maxDelaySamples=std::numeric_limits<std::uint64_t>::max();
        std::size_t memoryBudgetBytes=std::numeric_limits<std::size_t>::max();
    };

    /**
     * @brief Persistent mono delay: y[n]=x[n-D], with zero input before n=0.
     * @details Owns exactly D logical history samples, outside the temporary
     * BufferPool slots. Prepare/copy/move/destruction are control-only, after
     * quiescence; process belongs to one audio owner. Copy duplicates history
     * and cursor; move transfers them and leaves an empty pass-through source.
     * No backend pointer is retained. The default empty object implements D=0.
     */
    class DelayLine final {
    public:
        DelayLine() noexcept=default;
        DelayLine(const DelayLine&)=default;
        DelayLine& operator=(const DelayLine&)=default;
        DelayLine(DelayLine&& other) noexcept
            : history_(std::move(other.history_)),cursor_(std::exchange(other.cursor_,0)) {
            other.history_.clear();
        }
        DelayLine& operator=(DelayLine&& other) noexcept {
            if (this!=&other) {
                history_=std::move(other.history_);
                cursor_=std::exchange(other.cursor_,0);
                other.history_.clear();
            }
            return *this;
        }

        /// Prepare on control with no configured ceiling. Failure preserves all old state.
        [[nodiscard]] std::expected<void,PrepareError> prepare(const std::uint64_t delay) {
            return prepare(delay,DelayLineLimits{});
        }

        /**
         * @brief Validate sizes/quotas, allocate zero history, then commit atomically on control.
         * @details Checks size_t conversion, byte multiplication and container
         * addressability before allocation. Limits are borrowed for this call.
         * Budget/sample quota -> MemoryBudgetExceeded, unrepresentable storage
         * -> ArithmeticOverflow, allocation failure -> PrepareFailed. Success
         * resets cursor even if D is unchanged; no implicit reset per callback.
         */
        [[nodiscard]] std::expected<void,PrepareError> prepare(
            const std::uint64_t delay,const DelayLineLimits& limits) try {
            const auto bytes=checkedDelayBytes(delay,limits.maxDelaySamples);
            if (!bytes) return std::unexpected(bytes.error());
            if (delay>history_.max_size())
                return std::unexpected(PrepareError{ErrorCode::ArithmeticOverflow});
            if (*bytes>limits.memoryBudgetBytes)
                return std::unexpected(PrepareError{ErrorCode::MemoryBudgetExceeded});
            std::vector<float> candidate(static_cast<std::size_t>(delay),0.F);
            history_.swap(candidate);
            cursor_=0;
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PrepareError{ErrorCode::PrepareFailed});
        } catch (const std::length_error&) {
            return std::unexpected(PrepareError{ErrorCode::ArithmeticOverflow});
        }

        /**
         * @brief Process exactly frames samples, without allocation, locks or reset.
         * @pre For frames>0 both buffers cover frames samples. Source is read-only.
         * For D>0 their sample ranges are disjoint; the compiler proves this for
         * internal tasks. D=0 also permits the identical pointer. Partial overlap
         * is unsupported. frames=0 permits null pointers and touches no samples
         * or cursor. D is independent of callback size, including D>maxFrames.
         */
        void process(const float* source,float* destination,const FrameCount frames) noexcept {
            if (frames==0) return;
            if (history_.empty()) {
                if (source!=destination) std::copy_n(source,frames,destination);
                return;
            }
            for (FrameCount i=0;i<frames;++i) {
                destination[i]=history_[cursor_];
                history_[cursor_]=source[i];
                if (++cursor_==history_.size()) cursor_=0;
            }
        }

        /// Control-side immutable shape; do not inspect or replace an active owner's state.
        [[nodiscard]] std::uint64_t delaySamples() const noexcept { return history_.size(); }
        /// Requested history payload, excluding owner/allocator overhead.
        [[nodiscard]] std::size_t historyBytes() const noexcept { return history_.size()*sizeof(float); }

    private:
        std::vector<float> history_{};
        std::size_t cursor_=0;
    };
}
