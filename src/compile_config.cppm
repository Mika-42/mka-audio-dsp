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
        std::size_t maxNodes = std::numeric_limits<std::size_t>::max();
        std::size_t maxConnections = std::numeric_limits<std::size_t>::max();
        std::size_t memoryBudgetBytes = std::numeric_limits<std::size_t>::max();
    };
}
