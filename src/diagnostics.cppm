module;
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
export module mka.audio.dsp.diagnostics;
export import mka.audio.dsp.plan_switcher;
export namespace mka::audio::dsp {
    /// Stable category spelling. Unknown wire values are preserved by formatError.
    [[nodiscard]] constexpr std::string_view errorCodeName(ErrorCode code) noexcept {
        constexpr std::array<std::string_view,14> names{"InvalidParameter","NodeNotFound","InvalidPort","InvalidDirection",
            "ConnectionExists","InputOccupied","CycleDetected","NodeInUse","CapacityExceeded","ArithmeticOverflow",
            "MemoryBudgetExceeded","PrepareFailed","LatencyOverflow","PlanBusy"};
        const auto n=static_cast<std::uint16_t>(code);
        return n<names.size() ? names[n] : "UnknownErrorCode";
    }
    /** Format on control only: may allocate/throw, never call from process/exec. No logging side effect. */
    [[nodiscard]] std::string formatError(const Error& error) {
        return std::string(errorCodeName(error.code))+"("+std::to_string(static_cast<std::uint16_t>(error.code))+
            ") node="+std::to_string(error.node)+" channel="+std::to_string(error.channel);
    }
    /** Decode a drained numeric snapshot on control. Unknown bits remain visible numerically. */
    [[nodiscard]] std::string formatCallbackDiagnostics(std::uint32_t flags) {
        constexpr std::array<std::string_view,7> names{"PlanNotPrepared","FrameCapacityExceeded","ChannelCapacityExceeded",
            "NullBuffer","BufferCapacityExceeded","InvalidContext","CallbackAdmissionRejected"};
        std::string result;
        for (unsigned i=0;i<names.size();++i) if (flags&(1U<<i)) {
            if (!result.empty()) result+=" | "; result+=names[i]; flags&=~(1U<<i);
        }
        if (flags) { if (!result.empty()) result+=" | "; result+="UnknownBits="+std::to_string(flags); }
        return result.empty() ? "None" : result;
    }
}
