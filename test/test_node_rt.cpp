#include <array>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.node;
import mka.audio.dsp.dag;

namespace {
    // Constant-initialized thread-local counters avoid allocation in the probe.
    thread_local bool measuring = false;
    thread_local std::size_t allocations = 0;
    thread_local std::size_t deallocations = 0;
    thread_local bool failing = false;
    thread_local std::size_t allocationsBeforeFailure = 0;
    thread_local unsigned injectedFailures = 0;

    void checkAllocationFailure() {
        if (!failing) return;
        if (allocationsBeforeFailure == 0) {
            ++injectedFailures;
            throw std::bad_alloc{};
        }
        --allocationsBeforeFailure;
    }

    void* allocate(std::size_t bytes) {
        if (measuring) ++allocations;
        checkAllocationFailure();
        if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
        throw std::bad_alloc{};
    }

    void* allocateAligned(std::size_t bytes, const std::size_t alignment) {
        if (measuring) ++allocations;
        checkAllocationFailure();
        if (bytes == 0) bytes = 1;
        if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            throw std::bad_alloc{};
        }
        const auto rounded = ((bytes + alignment - 1) / alignment) * alignment;
        if (void* memory = std::aligned_alloc(alignment, rounded)) return memory;
        throw std::bad_alloc{};
    }

    void release(void* memory) noexcept {
        if (measuring && memory != nullptr) ++deallocations;
        std::free(memory);
    }

    struct AllocationWindow {
        AllocationWindow() noexcept {
            allocations = 0;
            deallocations = 0;
            measuring = true;
        }
        ~AllocationWindow() { measuring = false; }
    };

    struct FailureWindow {
        explicit FailureWindow(const std::size_t failAfter) noexcept {
            allocationsBeforeFailure = failAfter;
            injectedFailures = 0;
            failing = true;
        }
        ~FailureWindow() { failing = false; }
    };
}

// Replacements belong only to mka-node-rt-tests, never to the DSP library.
void* operator new(const std::size_t size) { return allocate(size); }
void* operator new[](const std::size_t size) { return allocate(size); }
void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try { return allocateAligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try { return allocateAligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* memory) noexcept { release(memory); }
void operator delete[](void* memory) noexcept { release(memory); }
void operator delete(void* memory, std::size_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { release(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { release(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { release(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { release(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { release(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { release(memory); }
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { release(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { release(memory); }

namespace {
    using namespace mka::audio::dsp;

    class GainNode final : public Node {
    public:
        explicit GainNode(const NodeID id = 1) : Node(id, Config{.layoutInput = {0}, .layoutAuxiliary = {},
                                    .layoutOutput = {0}}) {}

        void process(const ProcessContext& context) noexcept override {
            ++calls;
            backendChannels = context.backendOutputs.size();
            for (FrameCount i = 0; i < context.frames; ++i) {
                const auto sample = context.inputs[0].buffer[i];
                context.outputs[0].buffer[i] = sample * 2.F;
                context.backendOutputs[0].buffer[i] = sample + 100.F;
            }
        }

        unsigned calls = 0; // Numeric state only; no retained context or pointer.
        std::size_t backendChannels = 0;
    };

    TEST(NodeAllocationProbe, DetectsAnExplicitCppAllocationAndDeallocation) {
        {
            const AllocationWindow window;
            void* memory = ::operator new(32);
            ::operator delete(memory);
        }
        EXPECT_EQ(allocations, 1U);
        EXPECT_EQ(deallocations, 1U);
    }

    TEST(NodeRealtimeContract, ProcessAndContextBindingUseNoCppHeapOperations) {
        // All owning allocations/construction occur before measurement.
        GainNode node;
        std::array<float, 32> input{};
        std::array<float, 32> internalOutput{};
        std::array<float, 32> backendOutput{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i);
        const std::array inputs{InputChannel{0, input.data()}};
        const std::array outputs{OutputChannel{node.channel(PortType::Out, 0), internalOutput.data()}};
        const std::array backendOutputs{OutputChannel{0, backendOutput.data()}};
        const PreparedContextBindings bindings{.outputs = outputs, .maxFrames = 32,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        ASSERT_TRUE(validateContextBindings(bindings));
        Node& base = node;
        auto consume = [&base](const ProcessContext& context) noexcept { base.process(context); };
        bool accepted = true;
        {
            const AllocationWindow window;
            for (unsigned callback = 0; callback < 64; ++callback) {
                accepted = withProcessContext(bindings, inputs, backendOutputs, 32, consume) && accepted;
            }
        }
        EXPECT_TRUE(accepted);
        EXPECT_EQ(node.calls, 64U);
        EXPECT_EQ(allocations, 0U);
        EXPECT_EQ(deallocations, 0U);
        for (std::size_t i = 0; i < input.size(); ++i) {
            EXPECT_FLOAT_EQ(internalOutput[i], input[i] * 2.F);
            EXPECT_FLOAT_EQ(backendOutput[i], input[i] + 100.F);
        }
        // Destruction and its vector/token deallocations happen on control,
        // outside the measured process window.
    }

    TEST(BackendRealtimeContract, PreparedGraphRebindsAndProcessesWithoutCppHeapOperations) {
        DAG graph;
        const auto addedSource = graph.add<GainNode>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<GainNode*>(graph.node(*addedSource));
        const auto addedSink = graph.add<GainNode>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<GainNode*>(graph.node(*addedSink));
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(source.id()));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 32, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        std::array<float, 32> input{}, first{}, second{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i);
        const std::array<const float*, 1> inputs{input.data()};
        const std::array firstOutputs{first.data()};
        const std::array secondOutputs{second.data()};
        bool accepted = true;
        bool rejectedOverflow = false;
        {
            const AllocationWindow window;
            rejectedOverflow = !plan->exec(BackendBlock{inputs, firstOutputs, 33});
            for (unsigned callback = 0; callback < 64; ++callback) {
                const auto outputs = callback % 2 == 0 ? std::span<float* const>{firstOutputs} :
                                                        std::span<float* const>{secondOutputs};
                accepted = plan->exec(BackendBlock{inputs, outputs, 32}) && accepted;
            }
        }
        EXPECT_TRUE(rejectedOverflow);
        EXPECT_TRUE(accepted);
        EXPECT_EQ(source.calls, 64U);
        EXPECT_EQ(sink.calls, 64U);
        EXPECT_EQ(allocations, 0U);
        EXPECT_EQ(deallocations, 0U);
        for (std::size_t i = 0; i < input.size(); ++i) {
            EXPECT_FLOAT_EQ(first[i], input[i] * 2.F + 100.F);
            EXPECT_FLOAT_EQ(second[i], input[i] * 2.F + 100.F);
        }
    }

    struct ConstructionTrace {
        unsigned completed = 0;
        unsigned destroyed = 0;
    };
    class AllocationNode final : public Node {
    public:
        AllocationNode(const NodeID id, ConstructionTrace& trace)
            : Node(id, Config{.layoutInput = {0}, .layoutOutput = {0}}), trace_(trace) {
            ++trace_.completed;
        }
        ~AllocationNode() override { ++trace_.destroyed; }
        void process(const ProcessContext&) noexcept override {}
    private:
        ConstructionTrace& trace_;
    };

    TEST(DAGAllocationFailure, EveryAddAllocationRollsBackOwnershipAndIdentityWithoutChangingTheOldPlan) {
        std::size_t allocationPoints = 0;
        {
            ConstructionTrace trace;
            DAG graph;
            ASSERT_TRUE(graph.add<GainNode>());
            std::expected<NodeID, GraphError> added;
            {
                const AllocationWindow measured;
                added = graph.add<AllocationNode>(trace);
            }
            ASSERT_TRUE(added);
            allocationPoints = allocations;
        }
        ASSERT_GT(allocationPoints, 0U);
        bool observedPostConstructionCleanup = false;
        for (std::size_t failAt = 0; failAt < allocationPoints; ++failAt) {
            SCOPED_TRACE(failAt);
            ConstructionTrace trace;
            DAG graph;
            const auto first = graph.add<GainNode>();
            ASSERT_TRUE(first);
            auto* const originalAddress = graph.node(*first);
            ASSERT_TRUE(graph.markAsOutput(*first));
            auto oldPlan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1});
            ASSERT_TRUE(oldPlan);
            std::expected<NodeID, GraphError> failed;
            {
                const FailureWindow fault{failAt};
                failed = graph.add<AllocationNode>(trace);
            }
            ASSERT_FALSE(failed);
            EXPECT_EQ(failed.error(), GraphError::AllocationFailed);
            EXPECT_EQ(injectedFailures, 1U);
            EXPECT_EQ(trace.completed, trace.destroyed);
            observedPostConstructionCleanup = observedPostConstructionCleanup || trace.completed == 1;
            EXPECT_EQ(graph.node(2), nullptr);
            EXPECT_EQ(graph.node(*first), originalAddress);
            ASSERT_EQ(oldPlan->tasks().size(), 1U);
            EXPECT_EQ(oldPlan->tasks().front(), originalAddress);
            const float input = 3;
            float output = 99;
            const std::array inputs{&input};
            const std::array outputs{&output};
            ASSERT_TRUE(oldPlan->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 103);
            const auto retry = graph.add<AllocationNode>(trace);
            ASSERT_TRUE(retry);
            EXPECT_EQ(*retry, 2U);
            EXPECT_EQ(trace.completed - trace.destroyed, 1U);
        }
        // At least one failure was after the complete derived object existed,
        // exercising vector growth rollback and polymorphic candidate cleanup.
        EXPECT_TRUE(observedPostConstructionCleanup);
    }

    TEST(DAGAllocationFailure, EveryConnectionAllocationPreservesTheGraphAndOldPlanOnFailure) {
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        std::size_t allocationPoints = 0;
        {
            DAG graph;
            const auto source = graph.add<GainNode>();
            const auto sink = graph.add<GainNode>();
            ASSERT_TRUE(source);
            ASSERT_TRUE(sink);
            ASSERT_TRUE(graph.markAsOutput(*source));
            ASSERT_TRUE(graph.markAsOutput(*sink));
            auto oldPlan = graph.compile(config);
            ASSERT_TRUE(oldPlan);
            const auto from = graph.node(*source)->channel(PortType::Out, 0);
            const auto to = graph.node(*sink)->channel(PortType::In, 0);
            Result connected;
            {
                const AllocationWindow measured;
                connected = graph.connect(to, from);
            }
            ASSERT_TRUE(connected);
            allocationPoints = allocations;
        }
        ASSERT_GT(allocationPoints, 0U);
        for (std::size_t failAt = 0; failAt < allocationPoints; ++failAt) {
            SCOPED_TRACE(failAt);
            DAG graph;
            const auto source = graph.add<GainNode>();
            const auto sink = graph.add<GainNode>();
            ASSERT_TRUE(source);
            ASSERT_TRUE(sink);
            Node* const sourceAddress = graph.node(*source);
            Node* const sinkAddress = graph.node(*sink);
            const auto from = sourceAddress->channel(PortType::Out, 0);
            const auto to = sinkAddress->channel(PortType::In, 0);
            ASSERT_TRUE(graph.markAsOutput(*source));
            ASSERT_TRUE(graph.markAsOutput(*sink));
            auto oldPlan = graph.compile(config);
            ASSERT_TRUE(oldPlan);
            Result failed;
            {
                const FailureWindow fault{failAt};
                failed = graph.connect(to, from);
            }
            ASSERT_FALSE(failed);
            EXPECT_EQ(failed.error(), GraphError::AllocationFailed);
            EXPECT_EQ(injectedFailures, 1U);
            EXPECT_TRUE(graph.connections().empty());
            EXPECT_EQ(graph.node(*source), sourceAddress);
            EXPECT_EQ(graph.node(*sink), sinkAddress);
            ASSERT_EQ(oldPlan->tasks().size(), 2U);
            EXPECT_EQ(oldPlan->tasks()[0], sourceAddress);
            EXPECT_EQ(oldPlan->tasks()[1], sinkAddress);
            const float input = 3;
            float output = 99;
            const std::array inputs{&input};
            const std::array outputs{&output};
            ASSERT_TRUE(oldPlan->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 103);
            auto unchanged = graph.compile(config);
            ASSERT_TRUE(unchanged);
            ASSERT_TRUE(unchanged->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 103);
            EXPECT_EQ(static_cast<GainNode*>(sourceAddress)->backendChannels, 1U);
            EXPECT_EQ(static_cast<GainNode*>(sinkAddress)->backendChannels, 1U);
            ASSERT_TRUE(graph.connect(to, from));
            ASSERT_EQ(graph.connections().size(), 1U);
            EXPECT_EQ(graph.connections()[0], (Connection{from, to}));
            // A successful desired-graph edit also leaves the old audio snapshot intact.
            ASSERT_TRUE(oldPlan->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 103);
            auto updated = graph.compile(config);
            ASSERT_TRUE(updated);
            ASSERT_TRUE(updated->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 106);
        }
    }

    TEST(DAGAllocationFailure, FailedRetirementPreservesNodeEdgesOutputRolesAndOldPlan) {
        DAG graph;
        const auto source = graph.add<GainNode>();
        const auto sink = graph.add<GainNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        Node* const address = graph.node(*source);
        const auto from = address->channel(PortType::Out, 0);
        const auto to = graph.node(*sink)->channel(PortType::In, 0);
        ASSERT_TRUE(graph.connect(to, from));
        ASSERT_TRUE(graph.markAsOutput(*source));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        auto oldPlan = graph.compile(config);
        ASSERT_TRUE(oldPlan);
        Result failed;
        {
            const FailureWindow fault{0};
            failed = graph.remove(*source);
        }
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error(), GraphError::AllocationFailed);
        EXPECT_EQ(injectedFailures, 1U);
        EXPECT_EQ(graph.node(*source), address);
        const auto duplicate = graph.connect(to, from);
        ASSERT_FALSE(duplicate);
        EXPECT_EQ(duplicate.error(), GraphError::ConnectionExists);
        auto unchanged = graph.compile(config);
        ASSERT_TRUE(unchanged);
        const float input = 3;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        ASSERT_TRUE(unchanged->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 106);
        EXPECT_EQ(static_cast<GainNode*>(address)->backendChannels, 1U);
        EXPECT_EQ(static_cast<GainNode*>(graph.node(*sink))->backendChannels, 1U);
        ASSERT_TRUE(graph.remove(*source));
        EXPECT_EQ(graph.node(*source), nullptr);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        ASSERT_TRUE(oldPlan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 106);
        auto replacement = graph.compile(config);
        ASSERT_TRUE(replacement);
        ASSERT_TRUE(replacement->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 103); // Sink is now a topological source in this plan.
    }

    TEST(OutputMarkerFailure, FailedMarkIsTransactionalAndRepeatedMarkAndUnmarkDoNotAllocate) {
        ConstructionTrace trace;
        DAG graph;
        auto id = graph.add<AllocationNode>(trace);
        ASSERT_TRUE(id);
        auto old = graph.compile(BackendConfig{.maxFrames=1,.outputChannels=1});
        ASSERT_TRUE(old);
        Result result;
        {
            const FailureWindow fault{0};
            result = graph.markAsOutput(*id);
        }
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(),GraphError::AllocationFailed);
        EXPECT_EQ(injectedFailures,1U);
        EXPECT_TRUE(graph.outputNodes().empty());
        float output=99;
        const std::array outputs{&output};
        ASSERT_TRUE(old->exec(BackendBlock{.outputs=outputs,.frames=1}));
        EXPECT_EQ(output,0);
        ASSERT_TRUE(graph.markAsOutput(*id));
        {
            const FailureWindow fault{0};
            result = graph.markAsOutput(*id);
        }
        EXPECT_TRUE(result);
        EXPECT_EQ(injectedFailures,0U);
        ASSERT_EQ(graph.outputNodes().size(),1U);
        EXPECT_EQ(graph.outputNodes()[0],*id);
        {
            const FailureWindow fault{0};
            result = graph.unmarkAsOutput(*id);
        }
        EXPECT_TRUE(result);
        EXPECT_EQ(injectedFailures,0U);
        EXPECT_TRUE(graph.outputNodes().empty());
    }

    TEST(ConfigAllocationFailure, EveryConfigCopyFailureKeepsOwnedLayoutsEdgesAndIdentity) {
        const Config next{.layoutInput={2,6},.layoutAuxiliary={2,6},.layoutOutput={2,6}};
        std::size_t points=0;
        {
            ConstructionTrace trace;
            DAG graph;
            auto id=graph.add<AllocationNode>(trace); ASSERT_TRUE(id);
            Result changed;
            {
                const AllocationWindow measured;
                changed=graph.setConfig(*id,next);
            }
            ASSERT_TRUE(changed); points=allocations;
        }
        ASSERT_GT(points,0U);
        for (std::size_t i=0;i<points;++i) {
            SCOPED_TRACE(i);
            ConstructionTrace trace;
            DAG graph;
            auto a=graph.add<AllocationNode>(trace), b=graph.add<AllocationNode>(trace);
            ASSERT_TRUE(a); ASSERT_TRUE(b);
            Node* const address=graph.node(*a);
            const Connection edge{encode(*a,PortType::Out,0),encode(*b,PortType::In,0)};
            ASSERT_TRUE(graph.connect(edge.to,edge.from)); ASSERT_TRUE(graph.markAsOutput(*a));
            const Config before=address->config();
            Result failed;
            {
                const FailureWindow fault{i};
                failed=graph.setConfig(*a,next);
            }
            ASSERT_FALSE(failed); EXPECT_EQ(failed.error(),GraphError::AllocationFailed);
            EXPECT_EQ(injectedFailures,1U); EXPECT_EQ(graph.node(*a),address);
            EXPECT_EQ(address->config().layoutInput,before.layoutInput);
            EXPECT_EQ(address->config().layoutAuxiliary,before.layoutAuxiliary);
            EXPECT_EQ(address->config().layoutOutput,before.layoutOutput);
            ASSERT_EQ(graph.connections().size(),1U); EXPECT_EQ(graph.connections()[0],edge);
            ASSERT_EQ(graph.outputNodes().size(),1U); EXPECT_EQ(graph.outputNodes()[0],*a);
            ASSERT_TRUE(graph.setConfig(*a,next));
            EXPECT_EQ(address->config().layoutInput,next.layoutInput);
            EXPECT_EQ(address->config().layoutAuxiliary,next.layoutAuxiliary);
            EXPECT_EQ(address->config().layoutOutput,next.layoutOutput);
        }
    }

    TEST(TopologyAllocationFailure, EveryAllocationFailureLeavesTheDesiredGraphAndOldPlanIntact) {
        std::size_t points=0;
        {
            ConstructionTrace trace;
            DAG graph;
            for (unsigned i=0;i<4;++i) ASSERT_TRUE(graph.add<AllocationNode>(trace));
            ASSERT_TRUE(graph.connect(encode(1,PortType::In,0),encode(2,PortType::Out,0)));
            ASSERT_TRUE(graph.markAsOutput(3));
            std::expected<std::vector<NodeID>,GraphError> order;
            {
                const AllocationWindow measured;
                order=topologicalOrder(graph);
            }
            ASSERT_TRUE(order); points=allocations;
        }
        ASSERT_GT(points,0U);
        for (std::size_t i=0;i<points;++i) {
            SCOPED_TRACE(i);
            ConstructionTrace trace;
            DAG graph;
            for (unsigned n=0;n<4;++n) ASSERT_TRUE(graph.add<AllocationNode>(trace));
            const Connection edge{encode(2,PortType::Out,0),encode(1,PortType::In,0)};
            ASSERT_TRUE(graph.connect(edge.to,edge.from)); ASSERT_TRUE(graph.markAsOutput(3));
            auto old=graph.compile(); ASSERT_TRUE(old);
            const auto* const address=graph.node(1);
            std::expected<std::vector<NodeID>,GraphError> failed;
            {
                const FailureWindow fault{i};
                failed=topologicalOrder(graph);
            }
            ASSERT_FALSE(failed); EXPECT_EQ(failed.error(),GraphError::AllocationFailed);
            EXPECT_EQ(injectedFailures,1U); EXPECT_EQ(graph.node(1),address);
            ASSERT_EQ(graph.connections().size(),1U); EXPECT_EQ(graph.connections()[0],edge);
            ASSERT_EQ(graph.outputNodes().size(),1U); EXPECT_EQ(graph.outputNodes()[0],3U);
            const std::array<NodeID,4> expected{2,1,3,4};
            ASSERT_EQ(old->tasks().size(),4U);
            for (std::size_t n=0;n<4;++n) EXPECT_EQ(old->tasks()[n]->id(),expected[n]);
            const auto retry=topologicalOrder(graph); ASSERT_TRUE(retry);
            ASSERT_EQ(retry->size(),4U);
            for (std::size_t n=0;n<4;++n) EXPECT_EQ((*retry)[n],expected[n]);
        }
    }

    TEST(StructuredErrorAllocation, BackendCompileCatchesEveryAllocationAndPreservesTheOldPreparedPlan) {
        const BackendConfig config{.maxFrames=1,.outputChannels=1};
        std::size_t points=0;
        {
            ConstructionTrace trace;
            DAG graph;
            auto id=graph.add<AllocationNode>(trace); ASSERT_TRUE(id);
            ASSERT_TRUE(graph.markAsOutput(*id));
            std::expected<ExecutionPlan,Error> compiled{std::unexpected(Error{ErrorCode::PrepareFailed})};
            {
                const AllocationWindow measured;
                compiled=graph.compile(config);
            }
            ASSERT_TRUE(compiled); points=allocations;
        }
        ASSERT_GT(points,0U);
        for (std::size_t i=0;i<points;++i) {
            SCOPED_TRACE(i);
            ConstructionTrace trace;
            DAG graph;
            auto id=graph.add<AllocationNode>(trace); ASSERT_TRUE(id);
            Node* const address=graph.node(*id);
            ASSERT_TRUE(graph.markAsOutput(*id));
            auto old=graph.compile(config); ASSERT_TRUE(old);
            std::expected<ExecutionPlan,Error> failed{std::unexpected(Error{ErrorCode::PrepareFailed})};
            {
                const FailureWindow fault{i};
                failed=graph.compile(config);
            }
            ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code,ErrorCode::MemoryBudgetExceeded);
            EXPECT_EQ(injectedFailures,1U); EXPECT_EQ(graph.node(*id),address);
            EXPECT_EQ(trace.destroyed,0U); EXPECT_TRUE(old->isBackendPrepared());
            ASSERT_EQ(old->tasks().size(),1U); EXPECT_EQ(old->tasks()[0],address);
            ASSERT_EQ(graph.outputNodes().size(),1U); EXPECT_EQ(graph.outputNodes()[0],*id);
            float output=99; const std::array outputs{&output};
            ASSERT_TRUE(old->exec(BackendBlock{.outputs=outputs,.frames=1}));
            EXPECT_EQ(output,0);
            auto retry=graph.compile(config); ASSERT_TRUE(retry);
        }
    }

    TEST(OutputMarkerRealtime, UnmarkKeepsTheOldRoleWithNoHeapOperationsDuringCallbacks) {
        DAG graph;
        auto id = graph.add<GainNode>();
        ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto old = graph.compile(BackendConfig{.maxFrames=1,.inputChannels=1,.outputChannels=1});
        ASSERT_TRUE(old);
        ASSERT_TRUE(graph.unmarkAsOutput(*id));
        const float input=3;
        float first=99, second=99;
        const std::array inputs{&input};
        const std::array firstOutputs{&first}, secondOutputs{&second};
        bool accepted=true;
        {
            const AllocationWindow measured;
            for (unsigned i=0; i<64; ++i)
                accepted = old->exec(BackendBlock{inputs,i%2 ? firstOutputs : secondOutputs,1}) && accepted;
        }
        EXPECT_TRUE(accepted);
        EXPECT_EQ(allocations,0U);
        EXPECT_EQ(deallocations,0U);
        EXPECT_EQ(first,103);
        EXPECT_EQ(second,103);
        EXPECT_TRUE(graph.outputNodes().empty());
    }
}
