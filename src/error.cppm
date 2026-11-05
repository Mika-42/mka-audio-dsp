/**
 * @file error.cppm
 * @brief Stable numeric error categories and expected-result aliases.
 * @author Mika
 */
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
        /// @brief A supplied value or context violates the required domain.
        InvalidParameter = 0,
        /// @brief A required logical node identity is absent.
        NodeNotFound = 1,
        /// @brief A port code is outside the supported main/output/auxiliary domain.
        InvalidPort = 2,
        /// @brief The proposed edge is not Out to In or Aux.
        InvalidDirection = 3,
        /// @brief The exact logical connection is already present.
        ConnectionExists = 4,
        /// @brief Another producer already feeds this complete destination channel.
        InputOccupied = 5,
        /// @brief The desired dependency relation contains a cycle.
        CycleDetected = 6,
        /// @brief A retained plan still borrows the Node whose configuration would change.
        NodeInUse = 7,
        /// @brief A configured count or representable identity capacity would be exceeded.
        CapacityExceeded = 8,
        /// @brief A size calculation cannot be represented without overflow.
        ArithmeticOverflow = 9,
        /// @brief Requested retained storage or history length exceeds its configured limit.
        MemoryBudgetExceeded = 10,
        /// @brief Control-side construction or preparation could not complete.
        PrepareFailed = 11,
        /// @brief A cumulative sample-time calculation would overflow.
        LatencyOverflow = 12,
        /// @brief The requested plan transition has no exclusively owned destination slot.
        PlanBusy = 13
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

        /// @brief Construct a numeric error with optional node and channel context.
        constexpr Error() noexcept = default;
        /// @brief Construct a numeric error with optional node and channel context.
        constexpr Error(const ErrorCode value, const NodeID nodeID = 0,
                        const ChannelID channelID = 0) noexcept
            : code(value), node(nodeID), channel(channelID) {}

        /// @brief Compare all value fields for equality.
        constexpr bool operator==(const Error&) const noexcept = default;
        /// @brief Compare the numeric error category only.
        constexpr bool operator==(const ErrorCode value) const noexcept {
            return code == value;
        }

        // Canonical source spellings retained on the shared Error value type.
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode InvalidParameter = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for NodeNotFound.
        static constexpr ErrorCode NodeNotFound = ErrorCode::NodeNotFound;
        /// @brief Compatibility spelling for InvalidPort.
        static constexpr ErrorCode InvalidPort = ErrorCode::InvalidPort;
        /// @brief Compatibility spelling for InvalidDirection.
        static constexpr ErrorCode InvalidDirection = ErrorCode::InvalidDirection;
        /// @brief Compatibility spelling for ConnectionExists.
        static constexpr ErrorCode ConnectionExists = ErrorCode::ConnectionExists;
        /// @brief Compatibility spelling for InputOccupied.
        static constexpr ErrorCode InputOccupied = ErrorCode::InputOccupied;
        /// @brief Compatibility spelling for CycleDetected.
        static constexpr ErrorCode CycleDetected = ErrorCode::CycleDetected;
        /// @brief Compatibility spelling for NodeInUse.
        static constexpr ErrorCode NodeInUse = ErrorCode::NodeInUse;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode CapacityExceeded = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for ArithmeticOverflow.
        static constexpr ErrorCode ArithmeticOverflow = ErrorCode::ArithmeticOverflow;
        /// @brief Compatibility spelling for MemoryBudgetExceeded.
        static constexpr ErrorCode MemoryBudgetExceeded = ErrorCode::MemoryBudgetExceeded;
        /// @brief Compatibility spelling for PrepareFailed.
        static constexpr ErrorCode PrepareFailed = ErrorCode::PrepareFailed;
        /// @brief Compatibility spelling for LatencyOverflow.
        static constexpr ErrorCode LatencyOverflow = ErrorCode::LatencyOverflow;
        /// @brief Compatibility spelling for PlanBusy.
        static constexpr ErrorCode PlanBusy = ErrorCode::PlanBusy;

        // Compatibility names map into the canonical families, never new codes.
        /// @brief Compatibility spelling for ConnectionExists.
        static constexpr ErrorCode ConnexionAlreadyExists = ErrorCode::ConnectionExists;
        /// @brief Compatibility spelling for InputOccupied.
        static constexpr ErrorCode ChannelAlreadyConnected = ErrorCode::InputOccupied;
        /// @brief Compatibility spelling for MemoryBudgetExceeded.
        static constexpr ErrorCode AllocationFailed = ErrorCode::MemoryBudgetExceeded;
        /// @brief Compatibility spelling for PrepareFailed.
        static constexpr ErrorCode NodeConstructionFailed = ErrorCode::PrepareFailed;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode NodeIDExhausted = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode NodeCapacityExceeded = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode ConnectionCapacityExceeded = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode PreparationCapacityExceeded = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for CapacityExceeded.
        static constexpr ErrorCode LayoutCapacityExceeded = ErrorCode::CapacityExceeded;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode InvalidChannelIndex = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode NullBuffer = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode DuplicateChannelIndex = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode InvalidNodeIdentity = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode InvalidExecutionOrder = ErrorCode::InvalidParameter;
        /// @brief Compatibility spelling for InvalidParameter.
        static constexpr ErrorCode ConnexionNotFound = ErrorCode::InvalidParameter;
    };

    /// Domain aliases share one representation, context and ownership contract.
    using GraphError = Error;
    /// @brief Compilation failure using the shared structured Error representation.
    using CompileError = Error;
    /// @brief Preparation failure using the shared structured Error representation.
    using PrepareError = Error;
    /// Compatibility type spelling; now a value with .code/.node/.channel.
    using ErrorType = Error;
    /// @brief Success or a structured numeric error, without an allocated diagnostic message.
    using Result = std::expected<void, Error>;
}
