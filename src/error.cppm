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
        /// Legacy refusal code; retained as DAG now supports deferred Node retirement.
        NodeInUse,
        /// Prepared storage sizes cannot be represented safely by this platform.
        PreparationCapacityExceeded,
        /// The supplied task sequence is not a unique producer-before-consumer order.
        InvalidExecutionOrder,
        /// The DAG has issued every available 32-bit NodeID; identities never wrap.
        NodeIDExhausted,
        /// The DAG's owned-node quota or storage capacity has been reached.
        NodeCapacityExceeded,
        /// A control-side allocation failed; the requested mutation was not committed.
        AllocationFailed,
        /// A derived Node constructor threw a non-allocation exception.
        NodeConstructionFailed,
        /// A derived constructor did not preserve the NodeID assigned by the DAG.
        InvalidNodeIdentity,
        /// The control-side collection of logical edges cannot grow further.
        ConnectionCapacityExceeded
    };

    /**
     * @brief Graph-operation error vocabulary, sharing the existing error codes.
     * @details Alias, not a second enum: existing Result/ErrorType callers stay
     * compatible with std::expected<void, GraphError> and InvalidDirection.
     */
    using GraphError = ErrorType;

    using Result = std::expected<void, ErrorType>;

}
