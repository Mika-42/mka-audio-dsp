module;

#include <bitset>
#include <cstddef>
#include <expected>
#include <vector>

export module mka.audio.dsp.config;
export import mka.audio.dsp.types;

export namespace mka::audio::dsp {
    /// Maximum distinct ChannelIndex values per logical port, including 0/65535.
    inline constexpr std::size_t maxLayoutChannels = 65536;

    /**
     * @brief Ordered channel numbers a node actually processes on each port.
     * @details Each vector owns its values. Order is preserved; duplicates are
     * forbidden within one port, while the same index may occur on other ports.
     * These logical layouts are not connection allowlists or backend mappings.
     * Structurally valid DAG edges may refer to channels absent from a layout.
     * Topological sources must still receive all backend inputs, even with an
     * empty layoutInput; that binding belongs to the context/backend integration.
     * @note Construct, copy, edit and validate on control: vector operations can
     * allocate and throw standard allocation errors. Never reallocate during
     * process or mutate a configuration concurrently used by an active plan.
     * Check raw wide integers with toChannelIndex before inserting/narrowing.
     */
    struct Config {
        std::vector<ChannelIndex> layoutInput; ///< Main input numbers, in context order.
        std::vector<ChannelIndex> layoutAuxiliary; ///< Auxiliary input numbers, in context order.
        std::vector<ChannelIndex> layoutOutput; ///< Output numbers, in context order.
    };
}

namespace mka::audio::dsp {
    namespace {
        Result validateLayout(const std::vector<ChannelIndex>& layout) noexcept {
            if (layout.size() > maxLayoutChannels) {
                return std::unexpected(ErrorType::LayoutCapacityExceeded);
            }

            // The fixed 16-bit domain gives O(n) duplicate detection without
            // allocating or sorting/mutating the caller's ordered values.
            std::bitset<maxLayoutChannels> seen;
            for (const auto index : layout) {
                if (seen[index]) {
                    return std::unexpected(ErrorType::DuplicateChannelIndex);
                }
                seen[index] = true;
            }
            return {};
        }
    }
}

export namespace mka::audio::dsp {
    /**
     * @brief Validate layout capacity and uniqueness, without modifying Config.
     * @return ErrorType::LayoutCapacityExceeded for a port with more than 65536
     * entries, ErrorType::DuplicateChannelIndex for a repeated index in a port,
     * or success. Empty layouts and all ChannelIndex values are valid.
     * @details Ports are checked in input/auxiliary/output order. Within a port,
     * capacity is checked before duplicates. Counts use size_t, not ChannelIndex.
     * This checks the intrinsic index domain, not backend/hardware capacities.
     * @note Control-side noexcept validation; no heap allocation, locks or I/O.
     * A fixed 65536-bit bitmap (8 KiB) is used for each port check. Construction
     * and copying of Config itself can allocate and are not declared noexcept.
     * No context binding or runtime callback validation is performed here.
     */
    [[nodiscard]] Result validateConfig(const Config& config) noexcept {
        if (const auto result = validateLayout(config.layoutInput); !result) {
            return result;
        }
        if (const auto result = validateLayout(config.layoutAuxiliary); !result) {
            return result;
        }
        return validateLayout(config.layoutOutput);
    }
}
