module;
#include <cstdint>
#include <expected>

export module mka.audio.dsp.error;
export import mka.audio.dsp.identifiers;

export namespace mka::audio::dsp {
    /**
     * @brief Fixed-width control-error categories shared by all domains.
     * @details A category contains no message or resource ownership. Context is
     * carried separately by Error. These fourteen canonical values are the
     * public vocabulary; legacy names below map to one of these categories.
     */
    enum class ErrorCode : std::uint16_t {
        InvalidParameter,
        NodeNotFound,
        InvalidPort,
        InvalidDirection,
        ConnectionExists,
        InputOccupied,
        CycleDetected,
        NodeInUse,
        CapacityExceeded,
        ArithmeticOverflow,
        MemoryBudgetExceeded,
        PrepareFailed,
        LatencyOverflow,
        PlanBusy
    };

    /**
     * @brief Lightweight value describing a control-side failure and its context.
     * @details Owns only numeric values: no Node, channel sample, string, pointer,
     * allocation, lock or dynamic message. Copying never extends a resource's
     * lifetime. Irrelevant/unknown node and channel fields remain zero. Produce
     * optional human-readable messages separately on control, never in process.
     * All constructors and comparisons are constexpr/noexcept; status boundaries
     * return expected<T, Error> without introducing callback ownership work.
     * @note Replaces the historical enum ABI: recompile consumers and inspect
     * .code for a category. The code constructor and static spellings provide
     * source compatibility for unexpected(ErrorType::Code), not binary or
     * old-enum-number compatibility. Equality of two Errors includes context;
     * comparison with ErrorCode checks only the category.
     */
    struct Error {
        ErrorCode code{}; ///< Failure category; no message construction.
        NodeID node{}; ///< Affected/attempted logical NodeID, zero if unknown.
        ChannelID channel{}; ///< Affected logical ChannelID, zero if inapplicable.

        constexpr Error() noexcept = default;
        constexpr Error(const ErrorCode value, const NodeID nodeID = 0,
                        const ChannelID channelID = 0) noexcept
            : code(value), node(nodeID), channel(channelID) {}

        constexpr bool operator==(const Error&) const noexcept = default;
        constexpr bool operator==(const ErrorCode value) const noexcept {
            return code == value;
        }

        // Canonical source spellings retained on the shared Error value type.
        static constexpr ErrorCode InvalidParameter = ErrorCode::InvalidParameter;
        static constexpr ErrorCode NodeNotFound = ErrorCode::NodeNotFound;
        static constexpr ErrorCode InvalidPort = ErrorCode::InvalidPort;
        static constexpr ErrorCode InvalidDirection = ErrorCode::InvalidDirection;
        static constexpr ErrorCode ConnectionExists = ErrorCode::ConnectionExists;
        static constexpr ErrorCode InputOccupied = ErrorCode::InputOccupied;
        static constexpr ErrorCode CycleDetected = ErrorCode::CycleDetected;
        static constexpr ErrorCode NodeInUse = ErrorCode::NodeInUse;
        static constexpr ErrorCode CapacityExceeded = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode ArithmeticOverflow = ErrorCode::ArithmeticOverflow;
        static constexpr ErrorCode MemoryBudgetExceeded = ErrorCode::MemoryBudgetExceeded;
        static constexpr ErrorCode PrepareFailed = ErrorCode::PrepareFailed;
        static constexpr ErrorCode LatencyOverflow = ErrorCode::LatencyOverflow;
        static constexpr ErrorCode PlanBusy = ErrorCode::PlanBusy;

        // Compatibility names map into the canonical families, never new codes.
        static constexpr ErrorCode ConnexionAlreadyExists = ErrorCode::ConnectionExists;
        static constexpr ErrorCode ChannelAlreadyConnected = ErrorCode::InputOccupied;
        static constexpr ErrorCode AllocationFailed = ErrorCode::MemoryBudgetExceeded;
        static constexpr ErrorCode NodeConstructionFailed = ErrorCode::PrepareFailed;
        static constexpr ErrorCode NodeIDExhausted = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode NodeCapacityExceeded = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode ConnectionCapacityExceeded = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode PreparationCapacityExceeded = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode LayoutCapacityExceeded = ErrorCode::CapacityExceeded;
        static constexpr ErrorCode InvalidChannelIndex = ErrorCode::InvalidParameter;
        static constexpr ErrorCode NullBuffer = ErrorCode::InvalidParameter;
        static constexpr ErrorCode DuplicateChannelIndex = ErrorCode::InvalidParameter;
        static constexpr ErrorCode InvalidNodeIdentity = ErrorCode::InvalidParameter;
        static constexpr ErrorCode InvalidExecutionOrder = ErrorCode::InvalidParameter;
        static constexpr ErrorCode ConnexionNotFound = ErrorCode::InvalidParameter;
    };

    /// Domain aliases share one representation, context and ownership contract.
    using GraphError = Error;
    using CompileError = Error;
    using PrepareError = Error;
    /// Compatibility type spelling; now a value with .code/.node/.channel.
    using ErrorType = Error;
    using Result = std::expected<void, Error>;
}
