/**
 * @file pdc_analysis.cppm
 * @brief Cumulative path latencies and logical-edge compensation analysis.
 * @author Mika
 */
module;
#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

export module mka.audio.dsp.pdc_analysis;
export import mka.audio.dsp.latency_snapshot;
export import mka.audio.dsp.connection;

export namespace mka::audio::dsp {
    /// Compensation on one complete logical Out -> In/Aux edge, in samples.
    struct ConnectionDelay {
        /// @brief Complete logical producer-to-consumer edge identity.
        Connection connection;
        /// @brief Whole-sample compensation assigned to this exact logical edge.
        std::uint64_t samples = 0;
        /// @brief Compare all value fields for equality.
        bool operator==(const ConnectionDelay&) const noexcept = default;
    };

    /// Overflow-checked sample-time addition; also supports wide accumulated inputs.
    [[nodiscard]] constexpr std::expected<std::uint64_t, CompileError>
    checkedLatencyAdd(const std::uint64_t arrival, const std::uint64_t intrinsic) noexcept {
        if (intrinsic>std::numeric_limits<std::uint64_t>::max()-arrival)
            return std::unexpected(CompileError{ErrorCode::LatencyOverflow});
        return arrival+intrinsic;
    }

    /// P(v), A(v) and L(v), in integer samples at the snapshot's rate.
    struct NodeLatency {
        /// @brief Logical identity of the processor described by this latency entry.
        NodeID node = invalidNodeID;
        /// @brief Algorithmic latency declared by the processor.
        LatencySamples intrinsic = 0;
        /// @brief Maximum cumulative latency of the processed input predecessors.
        std::uint64_t arrival = 0;
        /// @brief Arrival latency plus this processor's intrinsic latency.
        std::uint64_t cumulative = 0;
        /// @brief Compare all value fields for equality.
        bool operator==(const NodeLatency&) const noexcept = default;
    };

    /**
     * @brief Owned control-side analysis, in topological task order.
     * @details V1 aligns known logical In/Aux edges before the consumer runs.
     * Output markers do not identify physical lanes: each marked Node can
     * freely write every backendOutputs buffer. No global per-hardware-channel
     * alignment or final Node's arbitrary internal timing can be inferred.
     * To align signals routed to different physical lanes, join them through
     * a common logical Mix/router first. Physical Last Writer Wins and separate
     * logical storage for output Nodes with successors remain unchanged.
     */
    struct PdcAnalysis {
        /// @brief Owned P/A/L entries in the prepared topological order.
        std::vector<NodeLatency> nodes;
        /// Complete materialized audio-edge table, including D=0 direct links.
        /// Sorted by destination then source ChannelID, independent of insertion.
        std::vector<ConnectionDelay> edges;

        /// @brief Borrow the matching immutable metadata entry, or return null when it is absent.
        [[nodiscard]] const NodeLatency* find(const NodeID id) const noexcept {
            const auto it=std::find_if(nodes.begin(),nodes.end(),
                [id](const NodeLatency& value) { return value.node==id; });
            return it==nodes.end() ? nullptr : &*it;
        }
        /// @brief Borrow the matching immutable metadata entry, or return null when it is absent.
        [[nodiscard]] const ConnectionDelay* find(const Connection edge) const noexcept {
            const auto it=std::find_if(edges.begin(),edges.end(),
                [edge](const ConnectionDelay& value) { return value.connection==edge; });
            return it==edges.end() ? nullptr : &*it;
        }
    };

    /**
     * @brief Compute A=max L(pred), L=A+P and exact-edge D=A-L(source) on control.
     * @details Reads only owned intrinsic values and numeric graph identities,
     * never Nodes or DSP state. audioEdges contains only materialized Out to
     * processed In/Aux connections; structural edges outside Config still
     * constrain the supplied topological order but do not contribute audio.
     * Snapshot identities must match the order exactly, sorted and unique.
     * A source with no audio predecessor has A=0, including isolated Nodes.
     * @return LatencyOverflow before an unrepresentable sum; InvalidParameter
     * for invalid order/table; NodeNotFound for an absent audio-edge endpoint;
     * InvalidPort/InvalidDirection for malformed endpoints; PrepareFailed or
     * CapacityExceeded for control storage failures. No publication or RT work.
     */
    [[nodiscard]] std::expected<PdcAnalysis, CompileError> analyzeLatencies(
        const LatencySnapshot& snapshot, const std::span<const NodeID> order,
        const std::span<const Connection> audioEdges) try {
        if (snapshot.nodes.size()!=order.size())
            return std::unexpected(Error{ErrorCode::InvalidParameter});
        for (std::size_t i=0;i<snapshot.nodes.size();++i)
            if (snapshot.nodes[i].node==invalidNodeID ||
                (i && snapshot.nodes[i-1].node>=snapshot.nodes[i].node))
                return std::unexpected(Error{ErrorCode::InvalidParameter,snapshot.nodes[i].node});
        PdcAnalysis result;
        result.nodes.reserve(order.size());
        std::unordered_map<NodeID,std::size_t> positions;
        positions.reserve(order.size());
        for (std::size_t i=0;i<order.size();++i) {
            const auto* declared=snapshot.find(order[i]);
            if (!declared || !positions.emplace(order[i],i).second)
                return std::unexpected(Error{ErrorCode::InvalidParameter,order[i]});
            result.nodes.push_back({order[i],declared->samples,0,0});
        }
        std::vector<std::vector<std::size_t>> predecessors(order.size());
        for (const auto& edge : audioEdges) {
            if (!isValidPort(decodePort(edge.from)) || !isValidPort(decodePort(edge.to)))
                return std::unexpected(Error{ErrorCode::InvalidPort});
            if (!isOutputPort(decodePort(edge.from)) || !isInputPort(decodePort(edge.to)))
                return std::unexpected(Error{ErrorCode::InvalidDirection});
            const auto from=positions.find(decodeNode(edge.from)),to=positions.find(decodeNode(edge.to));
            if (from==positions.end())
                return std::unexpected(Error{ErrorCode::NodeNotFound,decodeNode(edge.from),edge.from});
            if (to==positions.end())
                return std::unexpected(Error{ErrorCode::NodeNotFound,decodeNode(edge.to),edge.to});
            if (from->second>=to->second)
                return std::unexpected(Error{ErrorCode::InvalidParameter,decodeNode(edge.to),edge.to});
            predecessors[to->second].push_back(from->second);
        }
        for (std::size_t i=0;i<order.size();++i) {
            auto& value=result.nodes[i];
            for (const auto pred : predecessors[i])
                value.arrival=std::max(value.arrival,result.nodes[pred].cumulative);
            const auto cumulative=checkedLatencyAdd(value.arrival,value.intrinsic);
            if (!cumulative) return std::unexpected(Error{cumulative.error().code,value.node});
            value.cumulative=*cumulative;
        }
        result.edges.reserve(audioEdges.size());
        for (const auto edge : audioEdges) {
            const auto source=result.nodes[positions.find(decodeNode(edge.from))->second].cumulative;
            const auto arrival=result.nodes[positions.find(decodeNode(edge.to))->second].arrival;
            // The same predecessor set established A, so source <= arrival.
            result.edges.push_back({edge,arrival-source});
        }
        std::sort(result.edges.begin(),result.edges.end(),[](const auto& a,const auto& b) {
            return a.connection.to==b.connection.to ? a.connection.from<b.connection.from :
                a.connection.to<b.connection.to;
        });
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }
}
