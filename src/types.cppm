module;

#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>

export module mka.audio.dsp.types;
export import mka.audio.dsp.error;

export namespace mka::audio::dsp {
    /**
     * @brief Logical node identity, expressed as an unsigned 32-bit value.
     * @details Zero is reserved for an invalid/unassigned identity. Live nodes
     * use [1, UINT32_MAX]. Identity allocation belongs to the control thread:
     * an allocator must reject exhaustion before narrowing or incrementing past
     * UINT32_MAX, never wrap to zero, and never reuse an identity while any plan
     * can still reference it. This alias does not allocate or manage identities.
     */
    using NodeID = std::uint32_t;

    /**
     * @brief Numeric identity of a logical channel, expressed on 64 bits.
     * @details This is neither a pointer nor a backend channel address. It owns
     * no memory and conveys no sample-buffer lifetime. Its encoding is specified
     * separately; introducing this alias does not encode or decode channels.
     */
    using ChannelID = std::uint64_t;

    /**
     * @brief Zero-based logical channel number, in [0, 65535], on 16 bits.
     * @details Check wider/signed integers with toChannelIndex() on the control
     * thread before narrowing. Both endpoints, including zero, are valid.
     */
    using ChannelIndex = std::uint16_t;

    /**
     * @brief Number of audio frames (samples per channel), on 32 bits.
     * @details Zero denotes an empty block and permits no sample access. This
     * alias defines the target backend-compatible contract; migrating the legacy
     * ProcessContext::frames field is part of the ProcessContext integration.
     * Validate backend sizes and prepared capacities on the control side before
     * narrowing; this type alone does not validate buffer lengths.
     */
    using FrameCount = std::uint32_t;

    /// Invalid/unassigned NodeID; never assigned to a live node.
    inline constexpr NodeID invalidNodeID = 0;
    /// Last representable live NodeID; allocating beyond it must fail, not wrap.
    inline constexpr NodeID maxNodeID = std::numeric_limits<NodeID>::max();
    /// Highest representable logical channel number (inclusive).
    inline constexpr ChannelIndex maxChannelIndex =
        std::numeric_limits<ChannelIndex>::max();

    static_assert(sizeof(NodeID) == 4 && sizeof(ChannelID) == 8 &&
                  sizeof(ChannelIndex) == 2 && sizeof(FrameCount) == 4);

    /**
     * @brief Convert an integer to a logical channel index without truncation.
     * @tparam Integer Any standard integral type; floating-point values, enums
     * and pointers are not accepted. Boolean false/true map to 0/1.
     * @param value Number to validate before performing the narrowing cast.
     * @return The exact value if it belongs to [0, 65535], otherwise
     * std::unexpected(ErrorType::InvalidChannelIndex), including for negatives.
     * @note Pure constexpr/noexcept operation: no allocation, shared state,
     * exceptions or I/O. Intended for validation on the control thread; audio
     * processing should consume already validated indices.
     */
    template<std::integral Integer>
    [[nodiscard]] constexpr std::expected<ChannelIndex, ErrorType>
    toChannelIndex(const Integer value) noexcept {
        if constexpr (std::signed_integral<Integer>) {
            if (value < 0) {
                return std::unexpected(ErrorType::InvalidChannelIndex);
            }
        }

        if constexpr (std::numeric_limits<Integer>::digits >
                      std::numeric_limits<ChannelIndex>::digits) {
            if (value > maxChannelIndex) {
                return std::unexpected(ErrorType::InvalidChannelIndex);
            }
        }

        return static_cast<ChannelIndex>(value);
    }
}
