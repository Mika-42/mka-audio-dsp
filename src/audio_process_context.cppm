/**
 * @file audio_process_context.cppm
 * @brief Borrowed physical audio buffers and callback capacities.
 * @author Mika
 */
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
        /// @brief Borrowed read-only physical input pointers, valid for this callback only.
        std::span<const float* const> inputs;
        /// @brief Borrowed writable physical output pointers, valid for this callback only.
        std::span<float* const> outputs;
        /// @brief Effective samples per channel for this processing block.
        dsp::FrameCount frames = 0;
        /// @brief Readable samples available in each non-null physical input buffer.
        dsp::FrameCount inputCapacityFrames = frames;
        /// @brief Writable samples available in each non-null physical output buffer.
        dsp::FrameCount outputCapacityFrames = frames;
    };
}
