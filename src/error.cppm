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
        InvalidChannelIndex,
        /// A connection is not Out -> In or Out -> Aux (undefined codes included).
        InvalidDirection,
        /// A consumed audio channel has a null buffer for a positive-frame block.
        NullBuffer,
        /// A logical channel index is repeated within a single port layout.
        DuplicateChannelIndex,
        /// A port layout exceeds the 65536 representable distinct channel indices.
        LayoutCapacityExceeded,
        /// A prepared execution plan still borrows this Node; retry after retirement.
        NodeInUse,
        /// Prepared storage sizes cannot be represented safely by this platform.
        PreparationCapacityExceeded,
        /// The supplied task sequence is not a unique producer-before-consumer order.
        InvalidExecutionOrder
    };

    /**
     * @brief Graph-operation error vocabulary, sharing the existing error codes.
     * @details Alias, not a second enum: existing Result/ErrorType callers stay
     * compatible with std::expected<void, GraphError> and InvalidDirection.
     */
    using GraphError = ErrorType;

    using Result = std::expected<void, ErrorType>;

}
