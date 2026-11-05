/**
 * @file test_control_diagnostics.cpp
 * @brief Tests for control diagnostics.
 * @author Mika
 */
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.diagnostics;
#include "v1_fixture.hpp"
namespace {
    using namespace mka::audio::dsp;
    /// @brief Test case: every stable category and unknown value keeps numeric context.
    TEST(ControlDiagnostics, EveryStableCategoryAndUnknownValueKeepsNumericContext) {
        for (unsigned i=0;i<14;++i) {
            const Error e{static_cast<ErrorCode>(i),42,encode(42,PortType::Aux,7)};
            const auto text=formatError(e);
            EXPECT_NE(text.find(errorCodeName(e.code)),std::string::npos);
            EXPECT_NE(text.find("node=42"),std::string::npos);
            EXPECT_NE(text.find(std::to_string(e.channel)),std::string::npos);
        }
        EXPECT_NE(formatError(Error{static_cast<ErrorCode>(65535)}).find("65535"),std::string::npos);
        for (unsigned i=0;i<7;++i) EXPECT_NE(formatCallbackDiagnostics(1U<<i),"None");
        EXPECT_EQ(formatCallbackDiagnostics(0),"None");
        EXPECT_NE(formatCallbackDiagnostics(1U<<31).find("2147483648"),std::string::npos);
    }
    /// @brief Test case: switcher flags survive retirement and drain without callback formatting.
    TEST(ControlDiagnostics, SwitcherFlagsSurviveRetirementAndDrainWithoutCallbackFormatting) {
        v1_test::Graph g; ASSERT_TRUE(g.make(0)); PlanSwitcher sw; v1_test::Bank bank; bank.fill(128);
        v1_test::callback(sw,bank.io(1));
        EXPECT_EQ(sw.takeCallbackDiagnostics(),64U);
        const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=6}; ASSERT_TRUE(sw.initialize(g.dag,cfg));
        v1_test::callback(sw,bank.io(129));
        auto wrong=bank.io(1,2); v1_test::callback(sw,wrong);
        auto null=bank.io(1); std::array<const float*,2> inputs{nullptr,bank.inputs[1]}; null.inputs=inputs;
        v1_test::callback(sw,null);
        EXPECT_EQ(sw.callbackDiagnostics(),2U|4U|8U);
        ASSERT_TRUE(sw.prepareAndPublish(g.dag,cfg)); v1_test::callback(sw,bank.io(0));
        ASSERT_TRUE(sw.retireAcknowledgedPlan());
        const auto flags=sw.takeCallbackDiagnostics(); EXPECT_EQ(flags,14U);
        EXPECT_NE(formatCallbackDiagnostics(flags).find("NullBuffer"),std::string::npos);
        EXPECT_EQ(sw.takeCallbackDiagnostics(),0U);
        EXPECT_EQ(g.trace.destroyedInAudio.load(),0U);
    }
    /// @brief Test case: concurrent control drain and invalid callbacks use only atomic flags.
    TEST(ControlDiagnostics, ConcurrentControlDrainAndInvalidCallbacksUseOnlyAtomicFlags) {
        PlanSwitcher sw; std::atomic<bool> running{true};
        std::thread audio([&] { v1_test::Bank bank; bank.fill(1); for (unsigned i=0;i<10000;++i) sw.exec(bank.io(1)); running.store(false); });
        std::uint32_t flags=0;
        while (running.load()) flags|=sw.takeCallbackDiagnostics();
        audio.join(); flags|=sw.takeCallbackDiagnostics(); EXPECT_EQ(flags,64U);
    }
}
