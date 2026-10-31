#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>

#include <gtest/gtest.h>

import mka.audio.dsp.node;

namespace {
    // Constant-initialized thread-local counters avoid allocation in the probe.
    thread_local bool measuring = false;
    thread_local std::size_t allocations = 0;
    thread_local std::size_t deallocations = 0;

    void* allocate(std::size_t bytes) {
        if (measuring) ++allocations;
        if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
        throw std::bad_alloc{};
    }

    void* allocateAligned(std::size_t bytes, const std::size_t alignment) {
        if (measuring) ++allocations;
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
        GainNode() : Node(1, Config{.layoutInput = {0}, .layoutAuxiliary = {},
                                    .layoutOutput = {0}}) {}

        void process(const ProcessContext& context) noexcept override {
            ++calls;
            for (FrameCount i = 0; i < context.frames; ++i) {
                const auto sample = context.inputs[0].buffer[i];
                context.outputs[0].buffer[i] = sample * 2.F;
                context.backendOutputs[0].buffer[i] = sample + 100.F;
            }
        }

        unsigned calls = 0; // Numeric state only; no retained context or pointer.
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
}
