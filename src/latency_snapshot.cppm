module;
#include <algorithm>
#include <cstdint>
#include <expected>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

export module mka.audio.dsp.latency_snapshot;
export import mka.audio.dsp.node;

export namespace mka::audio::dsp {
    /// One value captured on control, independent of Node DSP storage.
    struct IntrinsicLatency {
        NodeID node = invalidNodeID;
        LatencySamples samples = 0;
        bool operator==(const IntrinsicLatency&) const noexcept = default;
    };

    /**
     * @brief Owned declarations for one candidate, sorted by NodeID.
     * @details sampleRate is the effective backend rate (zero: unspecified).
     * No callback reads this table. Prepared plans expose only const access;
     * copied values are detached and cannot edit a pending/active generation.
     */
    struct LatencySnapshot {
        std::uint32_t sampleRate = 0;
        std::vector<IntrinsicLatency> nodes;

        [[nodiscard]] const IntrinsicLatency* find(const NodeID id) const noexcept {
            const auto it=std::lower_bound(nodes.begin(),nodes.end(),id,
                [](const IntrinsicLatency& entry,NodeID key) { return entry.node<key; });
            return it!=nodes.end() && it->node==id ? &*it : nullptr;
        }
    };
}

export namespace mka::audio::dsp::detail {
    /**
     * @brief Capture a complete candidate table on serialized control.
     * @pre Caller owns one structural plan borrow per Node, or Nodes are
     * entirely unborrowed/quiescent. Declarations/DSP remain stable throughout
     * the borrow epoch. Other unprepared borrows without a frozen declaration
     * are conservatively rejected, as they cannot establish safe DSP access.
     * @details Shared prepared Nodes reuse the frozen declaration at the same
     * rate, without invoking virtual methods on active DSP. A rate change or
     * uncertain sharing returns NodeInUse. Prepare a distinct Node for B, or
     * release all plan borrows and prepare the DSP at its new rate first.
     * Allocation/validation failure installs no partial frozen table.
     */
    [[nodiscard]] std::expected<LatencySnapshot, Error> captureLatencies(
        const std::span<Node* const> nodes, const std::uint32_t sampleRate) try {
        std::vector<Node*> ordered(nodes.begin(),nodes.end());
        for (const auto* node : ordered)
            if (!node || node->id()==invalidNodeID)
                return std::unexpected(Error{ErrorCode::InvalidParameter});
        std::sort(ordered.begin(),ordered.end(),
            [](const Node* a,const Node* b) { return a->id()<b->id(); });
        LatencySnapshot result{sampleRate,{}};
        result.nodes.reserve(ordered.size());
        for (const auto* node : ordered) {
            if (!result.nodes.empty() && result.nodes.back().node==node->id())
                return std::unexpected(Error{ErrorCode::InvalidParameter,node->id()});
            const auto latency=NodeBorrowAccess::readLatency(*node,sampleRate);
            if (!latency) return std::unexpected(latency.error());
            result.nodes.push_back({node->id(),*latency});
        }
        for (std::size_t i=0;i<ordered.size();++i)
            NodeBorrowAccess::freezeLatency(*ordered[i],sampleRate,result.nodes[i].samples);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }
}
