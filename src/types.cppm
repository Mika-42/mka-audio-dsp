module;

#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>

export module mka.audio.dsp.types;
export import mka.audio.dsp.error;

export namespace mka::audio::dsp {
    /**
     * @brief Convert an integer to a logical channel index without truncation.
     * @tparam Integer Any standard integral type; floating-point values, enums
     * and pointers are not accepted. Boolean false/true map to 0/1.
     * @param value Number to validate before performing the narrowing cast.
     * @return The exact value if it belongs to [0, 65535], otherwise
     * an Error with InvalidParameter (legacy InvalidChannelIndex), including
     * negatives. Node/channel context stays zero because this integer conversion
     * does not know a complete logical channel identity.
     * @note Pure constexpr/noexcept operation: no allocation, shared state,
     * exceptions or I/O. Intended for validation on the control thread; audio
     * processing should consume already validated indices.
     */
    template<std::integral Integer>
    [[nodiscard]] constexpr std::expected<ChannelIndex, Error>
    toChannelIndex(const Integer value) noexcept {
        if constexpr (std::signed_integral<Integer>) {
            if (value < 0) {
                return std::unexpected(Error{ErrorCode::InvalidParameter});
            }
        }

        if constexpr (std::numeric_limits<Integer>::digits >
                      std::numeric_limits<ChannelIndex>::digits) {
            if (value > maxChannelIndex) {
                return std::unexpected(Error{ErrorCode::InvalidParameter});
            }
        }

        return static_cast<ChannelIndex>(value);
    }
}
