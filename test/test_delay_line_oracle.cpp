#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.delay_line;

namespace {
    using namespace mka::audio::dsp;
    constexpr float guard=-9876.F;
    enum class Wave { Impulses,Ramp,Alternating };
    float sample(Wave wave,std::size_t n) {
        if (wave==Wave::Ramp) return static_cast<float>(static_cast<int>(n%257)-128)*0.125F;
        if (wave==Wave::Alternating) return n%2 ? -0.75F : 1.25F;
        if (n==0) return 1.F;
        if (n==64) return -2.F;
        if (n==255) return 3.F;
        if (n==512) return 5.F;
        if (n==1023) return -1.F;
        return 0.F;
    }

    std::vector<float> render(std::uint64_t delay,Wave wave,std::span<const FrameCount> partition) {
        constexpr std::size_t total=2048;
        DelayLine line;
        const auto prepared=line.prepare(delay);
        EXPECT_TRUE(prepared);
        if (!prepared) return {};
        std::vector<float> result(total);
        std::array<float,130> input{},output{};
        std::size_t n=0,block=0;
        while (n<total) {
            const auto frames=static_cast<FrameCount>(std::min<std::size_t>(partition[block%partition.size()],total-n));
            input.fill(guard); output.fill(guard);
            for (FrameCount f=0;f<frames;++f) input[f+1]=sample(wave,n+f);
            const auto original=input;
            line.process(input.data()+1,output.data()+1,frames);
            EXPECT_EQ(input,original)<<"input changed at absolute n="<<n;
            EXPECT_EQ(output.front(),guard);
            for (std::size_t i=frames+1;i<output.size();++i)
                EXPECT_EQ(output[i],guard)<<"write outside frames="<<frames<<" offset="<<i;
            for (FrameCount f=0;f<frames;++f) {
                const auto index=n+f;
                // Absolute-time formula, without circular buffers, shift
                // registers, production helpers or callback-dependent state.
                const auto expected=index<delay ? 0.F : sample(wave,index-static_cast<std::size_t>(delay));
                EXPECT_EQ(output[f+1],expected)<<"D="<<delay<<" n="<<index<<" frames="<<frames;
                result[index]=output[f+1];
            }
            if (frames==0) line.process(nullptr,nullptr,0);
            n+=frames; ++block;
        }
        return result;
    }

    TEST(DelayLineOracle, ImpulsesRampsAndAlternationMatchEachSampleForAllDelayAndBlockBoundaries) {
        const std::array<FrameCount,9> variable{0,1,2,7,128,0,2,1,7};
        for (const std::uint64_t delay : {0U,1U,3U,64U,256U,513U,1025U})
            for (const auto wave : {Wave::Impulses,Wave::Ramp,Wave::Alternating}) {
                SCOPED_TRACE(::testing::Message()<<"D="<<delay<<" wave="<<static_cast<int>(wave));
                EXPECT_EQ(render(delay,wave,variable).size(),2048U);
            }
    }

    TEST(DelayLineOracle, AbsoluteOutputIsInvariantUnderSingleSampleFixedAndVariablePartitions) {
        const std::array<FrameCount,1> single{1},fixed{128};
        const std::array<FrameCount,8> variable{0,7,1,128,2,0,7,2};
        for (const std::uint64_t delay : {0U,1U,3U,64U,256U,513U})
            for (const auto wave : {Wave::Impulses,Wave::Ramp,Wave::Alternating}) {
                SCOPED_TRACE(::testing::Message()<<"D="<<delay<<" wave="<<static_cast<int>(wave));
                const auto a=render(delay,wave,single),b=render(delay,wave,fixed),c=render(delay,wave,variable);
                ASSERT_EQ(a.size(),b.size()); ASSERT_EQ(a.size(),c.size());
                for (std::size_t n=0;n<a.size();++n) {
                    EXPECT_EQ(a[n],b[n])<<"n="<<n; EXPECT_EQ(a[n],c[n])<<"n="<<n;
                }
            }
    }

    TEST(DelayLineOracle, LongRunsOfEmptyNullBlocksNeverAdvanceAHistoryOrWriteDestination) {
        DelayLine line; ASSERT_TRUE(line.prepare(3));
        const std::array<float,4> first{1,2,3,4},second{5,6,7,8}; std::array<float,4> output{};
        line.process(first.data(),output.data(),4);
        output.fill(guard);
        for (unsigned block=0;block<1024;++block) {
            line.process(nullptr,nullptr,0); line.process(first.data(),output.data(),0);
        }
        EXPECT_EQ(output,(std::array<float,4>{guard,guard,guard,guard}));
        line.process(second.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
    }

    TEST(DelayLineOracle, ExplicitReprepareResetsOnlyAfterSuccessEvenAtExactBudgetAndOverflowLimits) {
        DelayLine line; ASSERT_TRUE(line.prepare(64,DelayLineLimits{.memoryBudgetBytes=64*sizeof(float)}));
        std::array<float,128> input{},output{};
        for (unsigned f=0;f<128;++f) input[f]=static_cast<float>(f+1);
        line.process(input.data(),output.data(),128);
        EXPECT_EQ(line.prepare(64,DelayLineLimits{.memoryBudgetBytes=64*sizeof(float)-1}).error().code,
                  ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(line.prepare(std::numeric_limits<std::size_t>::max()).error().code,ErrorCode::ArithmeticOverflow);
        for (unsigned f=0;f<128;++f) input[f]=static_cast<float>(f+129);
        line.process(input.data(),output.data(),128);
        for (unsigned f=0;f<128;++f) EXPECT_EQ(output[f],static_cast<float>(f+65))<<"n="<<f+128;
        ASSERT_TRUE(line.prepare(64));
        line.process(input.data(),output.data(),128);
        for (unsigned f=0;f<128;++f) EXPECT_EQ(output[f],f<64 ? 0.F : input[f-64]);
    }
}
