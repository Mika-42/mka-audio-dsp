/**
 * @file compile_config.cppm
 * @brief Control-side limits for structural graph compilation.
 * @author Mika
 */
module;
#include <cstddef>
#include <limits>

export module mka.audio.dsp.compile_config;

export namespace mka::audio::dsp {
    /**
     * @brief Control-side limits for one structural plan compilation.
     * @details Limits are inclusive. The byte budget counts the requested
     * element storage of the resulting structural plan, including its task,
     * index, layer and borrow arrays. It excludes allocator bookkeeping,
     * temporary compilation containers and any later audio preparation.
     * A default configuration imposes no additional application limit.
     */
    struct CompileConfig {
        /// @brief Inclusive limit on the number of Nodes admitted to structural compilation.
        std::size_t maxNodes = std::numeric_limits<std::size_t>::max();
        /// @brief Inclusive limit on logical connections admitted to structural compilation.
        std::size_t maxConnections = std::numeric_limits<std::size_t>::max();
        /// @brief Inclusive retained-storage budget in bytes.
        std::size_t memoryBudgetBytes = std::numeric_limits<std::size_t>::max();
    };
}
