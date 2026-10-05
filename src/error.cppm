//
// Created by mika on 10/5/26.
//
module;
#include <expected>
export module mka.audio.dsp.error;

export namespace mka::audio::dsp {
    enum class ErrorType {
        CycleDetected,
        NodeNotFound,
        EdgeAlreadyExists,
        EdgeNotFound
    };

    using Result = std::expected<void, ErrorType>;

}