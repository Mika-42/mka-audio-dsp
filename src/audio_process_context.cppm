module;
#include <span>

export module mka.audio.process_context;
export import mka.audio.dsp.types;

export namespace mka::audio {
    /**
     * @brief Borrowed physical buffers for one synchronous audio callback.
     * @details Each non-null pointer covers its corresponding capacity in
     * floats. Capacities default to frames, but may be explicitly smaller for
     * safe rejection of malformed blocks. Descriptor arrays remain valid for
     * the call; input samples are read-only, output samples are writable.
     * No ownership, channel encoding or device dependency. No sample access
     * for frames == 0. Inputs must not overlap writable internal/output memory.
     */
    struct AudioProcessContext {
        std::span<const float* const> inputs;
        std::span<float* const> outputs;
        dsp::FrameCount frames = 0;
        dsp::FrameCount inputCapacityFrames = frames;
        dsp::FrameCount outputCapacityFrames = frames;
    };
}
