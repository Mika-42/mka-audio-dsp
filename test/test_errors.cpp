#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class ErrorNode final : public Node {
    public:
        explicit ErrorNode(NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override {}
    };
    struct ConstructorFailure {};
    class FailingNode final : public Node {
    public:
        explicit FailingNode(NodeID id) : Node(id, Config{}) { throw ConstructorFailure{}; }
        void process(const ProcessContext&) noexcept override {}
    };

    constexpr std::array codes{
        ErrorCode::InvalidParameter, ErrorCode::NodeNotFound, ErrorCode::InvalidPort,
        ErrorCode::InvalidDirection, ErrorCode::ConnectionExists, ErrorCode::InputOccupied,
        ErrorCode::CycleDetected, ErrorCode::NodeInUse, ErrorCode::CapacityExceeded,
        ErrorCode::ArithmeticOverflow, ErrorCode::MemoryBudgetExceeded, ErrorCode::PrepareFailed,
        ErrorCode::LatencyOverflow, ErrorCode::PlanBusy};
    static_assert(std::is_scoped_enum_v<ErrorCode>);
    static_assert(std::same_as<std::underlying_type_t<ErrorCode>, std::uint16_t>);
    static_assert([] {
        for (std::size_t i = 0; i < codes.size(); ++i)
            if (static_cast<std::uint16_t>(codes[i]) != i) return false;
        return true;
    }());
    static_assert(std::same_as<decltype(Error::code), ErrorCode>);
    static_assert(std::same_as<decltype(Error::node), NodeID>);
    static_assert(std::same_as<decltype(Error::channel), ChannelID>);
    static_assert(std::same_as<GraphError, Error> && std::same_as<CompileError, Error> &&
                  std::same_as<PrepareError, Error> && std::same_as<ErrorType, Error>);
    static_assert(std::same_as<Result, std::expected<void, Error>>);
    static_assert(std::is_standard_layout_v<Error> && std::is_trivially_copyable_v<Error>);
    static_assert(std::is_trivially_destructible_v<Error> && !std::is_polymorphic_v<Error>);
    static_assert(std::is_nothrow_constructible_v<Error, ErrorCode, NodeID, ChannelID>);
    static_assert(noexcept(Error{} == Error{}));
    static_assert(Error{ErrorCode::InvalidPort, 7, 8} == Error{ErrorCode::InvalidPort, 7, 8});
    static_assert(Error{ErrorCode::InvalidPort, 7, 8} != Error{ErrorCode::InvalidPort, 9, 8});
    static_assert(Error{ErrorCode::InvalidPort, 7, 8} != Error{ErrorCode::InvalidPort, 7, 9});
    static_assert(Error{ErrorCode::InvalidPort, 7, 8} == ErrorCode::InvalidPort);
    static_assert(ErrorCode::InvalidPort == Error{ErrorCode::InvalidPort, 7, 8});

    /** Compare every diagnostic field, rather than only the compatibility category. */
    template<class T>
    void expectError(const std::expected<T, Error>& result, const Error& expected) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, expected.code);
        EXPECT_EQ(result.error().node, expected.node);
        EXPECT_EQ(result.error().channel, expected.channel);
        EXPECT_EQ(result.error(), expected);
    }

    TEST(StructuredErrors, EveryCanonicalFamilyRoundTripsThroughTheSharedExpectedValue) {
        constexpr NodeID node = 0xFEDCBA98U;
        constexpr ChannelID channel = 0xFEDCBA980001FFFFULL;
        for (const auto code : codes) {
            SCOPED_TRACE(static_cast<std::uint16_t>(code));
            const Error original{code, node, channel};
            const std::expected<NodeID, Error> graphResult = std::unexpected(original);
            const std::expected<NodeID, CompileError> compiled = graphResult;
            const std::expected<NodeID, PrepareError> prepared = compiled;
            ASSERT_NO_FATAL_FAILURE(expectError(prepared, original));
            Error detached = prepared.error();
            detached.node = 1;
            detached.channel = 2;
            EXPECT_EQ(original.node, node);
            EXPECT_EQ(original.channel, channel);
            EXPECT_EQ(prepared.error(), original);
        }
        const Error withoutContext{ErrorCode::PlanBusy};
        EXPECT_EQ(withoutContext.node, 0U);
        EXPECT_EQ(withoutContext.channel, 0U);
        // PlanBusy/latency/arithmetic are transported vocabulary, not invented subsystems.
        const Result legacy = std::unexpected(ErrorType::AllocationFailed);
        ASSERT_NO_FATAL_FAILURE(expectError(legacy, Error{ErrorCode::MemoryBudgetExceeded}));
    }

    TEST(StructuredErrors, ConnectionPortDirectionAndMissingEndpointKeepTheFullOffendingChannel) {
        DAG graph;
        const auto a = graph.add<ErrorNode>(), b = graph.add<ErrorNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const auto source = encode(*a, PortType::Out, 65535);
        const auto destination = encode(*b, PortType::In, 42);
        const ChannelID malformed = (ChannelID{*a} << 32U) | (ChannelID{0x8001} << 16U) | 65535;
        const auto wrongDirection = encode(*a, PortType::In, 65535);
        const auto missingSource = encode(maxNodeID, PortType::Out, 65535);
        const auto missingDestination = encode(maxNodeID, PortType::Aux, 65535);
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(destination, malformed),
            Error{ErrorCode::InvalidPort, *a, malformed}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(destination, wrongDirection),
            Error{ErrorCode::InvalidDirection, *a, wrongDirection}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(destination, missingSource),
            Error{ErrorCode::NodeNotFound, maxNodeID, missingSource}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(missingDestination, source),
            Error{ErrorCode::NodeNotFound, maxNodeID, missingDestination}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.disconnect(destination, malformed),
            Error{ErrorCode::InvalidDirection, *a, malformed}));
        const Channel wideNode{(std::uint64_t{1} << 32U) | *a, 0, PortType::Out};
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(Channel{*b, 42, PortType::In}, wideNode),
            Error{ErrorCode::NodeNotFound}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(Channel{*b, 42, PortType::In},
            Channel{*a, 65536, PortType::Out}), Error{ErrorCode::InvalidParameter, *a}));
        EXPECT_TRUE(graph.connections().empty());
        EXPECT_TRUE(graph.outputNodes().empty());
    }

    TEST(StructuredErrors, DuplicateOccupationAndCycleIdentifyTheDestinationWithoutMutation) {
        DAG graph;
        const auto a = graph.add<ErrorNode>(), b = graph.add<ErrorNode>(), c = graph.add<ErrorNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
        const Connection original{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 0)};
        ASSERT_TRUE(graph.connect(original.to, original.from));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(original.to, original.from),
            Error{ErrorCode::ConnectionExists, *b, original.to}));
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(original.to, encode(*c, PortType::Out, 65535)),
            Error{ErrorCode::InputOccupied, *b, original.to}));
        const auto back = encode(*a, PortType::Aux, 42);
        ASSERT_NO_FATAL_FAILURE(expectError(graph.connect(back, encode(*b, PortType::Out, 0)),
            Error{ErrorCode::CycleDetected, *a, back}));
        ASSERT_EQ(graph.connections().size(), 1U);
        EXPECT_EQ(graph.connections()[0], original);
    }

    TEST(StructuredErrors, NodeOperationsCarryIdentityAndConfigErrorsPreserveTheOwnedSnapshot) {
        DAG graph;
        const auto node = graph.add<ErrorNode>();
        ASSERT_TRUE(node);
        for (const auto missing : {invalidNodeID, maxNodeID}) {
            const Error expected{ErrorCode::NodeNotFound, missing};
            ASSERT_NO_FATAL_FAILURE(expectError(graph.markAsOutput(missing), expected));
            ASSERT_NO_FATAL_FAILURE(expectError(graph.unmarkAsOutput(missing), expected));
            ASSERT_NO_FATAL_FAILURE(expectError(graph.remove(missing), expected));
            ASSERT_NO_FATAL_FAILURE(expectError(graph.setConfig(missing, Config{}), expected));
        }
        const Config invalid{.layoutInput = {2, 2}};
        ASSERT_NO_FATAL_FAILURE(expectError(graph.setConfig(*node, invalid),
            Error{ErrorCode::InvalidParameter, *node}));
        auto old = graph.compile();
        ASSERT_TRUE(old);
        ASSERT_NO_FATAL_FAILURE(expectError(graph.setConfig(*node, Config{.layoutInput = {2, 6}}),
            Error{ErrorCode::NodeInUse, *node}));
        EXPECT_TRUE(graph.node(*node)->config().layoutInput.empty());
        ASSERT_EQ(old->tasks().size(), 1U);
        EXPECT_EQ(old->tasks()[0], graph.node(*node));
        EXPECT_TRUE(graph.connections().empty());
        EXPECT_TRUE(graph.outputNodes().empty());
    }

    TEST(StructuredErrors, ConstructionAndQuotaFailuresKeepTheAttemptedIdentityWithoutConsumingIt) {
        DAG disabled{0};
        ASSERT_NO_FATAL_FAILURE(expectError(disabled.add<ErrorNode>(),
            Error{ErrorCode::CapacityExceeded, 1}));
        EXPECT_EQ(disabled.node(1), nullptr);
        DAG graph;
        ASSERT_NO_FATAL_FAILURE(expectError(graph.add<FailingNode>(),
            Error{ErrorCode::PrepareFailed, 1}));
        EXPECT_EQ(graph.node(1), nullptr);
        const auto retry = graph.add<ErrorNode>();
        ASSERT_TRUE(retry);
        EXPECT_EQ(*retry, 1U);
    }

    TEST(StructuredErrors, PreparationErrorsCarryLinkOrMarkerContextAndPreserveTheOldPreparedState) {
        DAG graph;
        const auto a = graph.add<ErrorNode>(), b = graph.add<ErrorNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const BackendConfig config{.maxFrames = 1, .outputChannels = 1};
        auto plan = graph.compile(config);
        ASSERT_TRUE(plan);
        const auto malformed = (ChannelID{*a} << 32U) | (ChannelID{0x8001} << 16U);
        const std::array badPort{Connection{malformed, encode(*b, PortType::In, 0)}};
        ASSERT_NO_FATAL_FAILURE(expectError(plan->prepareBackend(config, badPort, {}),
            Error{ErrorCode::InvalidPort, *a, malformed}));
        const auto absent = encode(maxNodeID, PortType::Out, 65535);
        const std::array missingLink{Connection{absent, encode(*b, PortType::In, 0)}};
        ASSERT_NO_FATAL_FAILURE(expectError(plan->prepareBackend(config, missingLink, {}),
            Error{ErrorCode::NodeNotFound, maxNodeID, absent}));
        const std::array missingMarker{maxNodeID};
        ASSERT_NO_FATAL_FAILURE(expectError(plan->prepareBackend(config, {}, missingMarker),
            Error{ErrorCode::NodeNotFound, maxNodeID}));
        const BackendConfig excessive{.outputChannels = std::numeric_limits<std::size_t>::max()};
        ASSERT_NO_FATAL_FAILURE(expectError(plan->prepareBackend(excessive, {}, {}),
            Error{ErrorCode::CapacityExceeded}));
        EXPECT_TRUE(plan->isBackendPrepared());
        ASSERT_EQ(plan->tasks().size(), 2U);
        float sample = 99;
        const std::array outputs{&sample};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(sample, 0);
    }

    TEST(StructuredErrors, NullLogicalSamplesKeepTheChannelWhilePhysicalViewsKeepZeroContext) {
        const auto input = encode(NodeID{0xFEDCBA98U}, PortType::In, 65535);
        const auto output = encode(NodeID{0x80000001U}, PortType::Out, 42);
        const std::array inputs{InputChannel{input, nullptr}};
        const std::array outputs{OutputChannel{output, nullptr}};
        ASSERT_NO_FATAL_FAILURE(expectError(validateChannelBuffers(inputs, 1),
            Error{ErrorCode::InvalidParameter, NodeID{0xFEDCBA98U}, input}));
        ASSERT_NO_FATAL_FAILURE(expectError(validateChannelBuffers(outputs, 1),
            Error{ErrorCode::InvalidParameter, NodeID{0x80000001U}, output}));
        const std::array physical{InputChannel{0, nullptr}};
        ASSERT_NO_FATAL_FAILURE(expectError(validateChannelBuffers(physical, 1),
            Error{ErrorCode::InvalidParameter}));
        EXPECT_TRUE(validateChannelBuffers(inputs, 0));
    }
}
