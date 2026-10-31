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
        ConnexionAlreadyExists,
        ConnexionNotFound,
        ChannelAlreadyConnected,
        InvalidPort,
        /// An integer cannot be represented as a ChannelIndex (0..65535).
        InvalidChannelIndex
    };

    using Result = std::expected<void, ErrorType>;

}
