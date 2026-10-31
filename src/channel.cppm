//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>
#include <expected>
#include <span>

export module mka.audio.dsp.channel;
export import mka.audio.dsp.types;

export namespace mka::audio::dsp {

    namespace Port {
        constexpr std::uint32_t FL   = 0;
        constexpr std::uint32_t FR   = 1;
        constexpr std::uint32_t FC   = 2;
        constexpr std::uint32_t LFE  = 3;
        constexpr std::uint32_t SL   = 4;
        constexpr std::uint32_t SR   = 5;
        constexpr std::uint32_t BL   = 6;
        constexpr std::uint32_t BR   = 7;
    }

    /**
     * @brief Logical audio port category, stored on exactly 16 bits.
     * @details The numeric codes are fixed: In=0, Out=1, Aux=2. Aux is an
     * auxiliary/sidechain input; its signal interpretation belongs to the node.
     * Connections are exclusively Out -> In or Out -> Aux. These logical ports
     * neither select backend channels nor introduce hardware routing.
     */
    enum class PortType : std::uint16_t {
        In = 0,  ///< Main input, read by the node.
        Out = 1, ///< Output produced by the node.
        Aux = 2  ///< Auxiliary input, read by the node.
    };

    /**
     * @brief Whether a port can consume a logical connection (In or Aux).
     * @return False for Out and every undefined 16-bit port code.
     * @note Pure constexpr/noexcept query: no allocation, I/O or shared state.
     */
    [[nodiscard]] constexpr bool isInputPort(const PortType port) noexcept {
        return port == PortType::In || port == PortType::Aux;
    }

    /**
     * @brief Whether a port can produce a logical connection (Out only).
     * @return False for In, Aux and every undefined 16-bit port code.
     * @note Pure constexpr/noexcept query: no allocation, I/O or shared state.
     */
    [[nodiscard]] constexpr bool isOutputPort(const PortType port) noexcept {
        return port == PortType::Out;
    }

    /**
     * @brief Whether the represented 16-bit code is one of In, Out or Aux.
     * @return False for every undefined code, including codes obtained by cast.
     * @note Pure constexpr/noexcept query. Validate decoded values on the
     * control thread before using them; Node::process adds no dynamic checks.
     */
    [[nodiscard]] constexpr bool isValidPort(const PortType port) noexcept {
        return isInputPort(port) || isOutputPort(port);
    }

    /**
     * @brief Pack a logical channel into a numeric 64-bit identity.
     * @param node Node field, including all representable 32-bit values.
     * @param port Logical port category; must be In, Out or Aux.
     * @param index Logical channel number in [0, 65535], not a backend lane.
     * @pre isValidPort(port). Validate untrusted/decoded ports on control first.
     * @details Numeric format: bits 63..32 = node, 31..16 = port, 15..0 = index.
     * Encoding node zero is allowed as a numeric representation; it neither
     * allocates an identity nor makes zero a valid live-node identity.
     * The value owns no memory, is never an address and proves no node/buffer
     * lifetime. Its bit layout is independent of host endianness; writing raw
     * object bytes is not a portable serialization without an agreed byte order.
     * @note Pure constexpr/noexcept operation; no allocation, I/O or shared state.
     */
    [[nodiscard]] constexpr ChannelID encode(const NodeID node, const PortType port,
                                            const ChannelIndex index) noexcept {
        return (static_cast<ChannelID>(node) << 32U) |
               (static_cast<ChannelID>(static_cast<std::uint16_t>(port)) << 16U) |
               static_cast<ChannelID>(index);
    }

    /**
     * @brief Extract bits 63..32 as a NodeID, without checking node existence.
     * @note Pure constexpr/noexcept query for any ChannelID, including zero.
     */
    [[nodiscard]] constexpr NodeID decodeNode(const ChannelID id) noexcept {
        return static_cast<NodeID>((id >> 32U) & ChannelID{0xFFFFFFFF});
    }

    /**
     * @brief Extract bits 31..16 as a PortType, preserving undefined codes.
     * @return The exact 16-bit field, which can be outside In/Out/Aux.
     * @warning Check isValidPort() before using a decoded port or passing it to
     * encode(). Decoding never normalizes an undefined code to an official port.
     * @note Pure constexpr/noexcept query for any ChannelID; no allocation.
     */
    [[nodiscard]] constexpr PortType decodePort(const ChannelID id) noexcept {
        return static_cast<PortType>((id >> 16U) & ChannelID{0xFFFF});
    }

    /**
     * @brief Extract bits 15..0 as a logical ChannelIndex in [0, 65535].
     * @note Pure constexpr/noexcept query; no backend lookup or memory ownership.
     */
    [[nodiscard]] constexpr ChannelIndex decodeChannel(const ChannelID id) noexcept {
        return static_cast<ChannelIndex>(id & ChannelID{0xFFFF});
    }

    /**
     * @brief Non-owning read-only view of one channel's float samples.
     * @details For internal DAG data, channel identifies a logical channel and
     * the prepared plan owns the samples. For backend data, channel is zero and
     * ignored: the descriptor's position in its span selects the physical lane.
     * Backend samples remain backend-owned and valid only during that callback.
     * The owner must rebind descriptors for each callback; a Node must not retain
     * a backend pointer. Copying/destroying a descriptor never manages samples.
     * @pre If consumed with frames > 0, buffer points to at least frames valid
     * floats. Disconnected inputs use borrowed silence storage, never nullptr.
     * No sample may be read for frames == 0. Null is allowed during preparation.
     */
    struct InputChannel {
        ChannelID channel{}; ///< Logical identity; zero/ignored for backend views.
        const float* buffer = nullptr; ///< Borrowed samples; no writes through this pointer.
    };

    /**
     * @brief Non-owning writable view of one channel's float samples.
     * @details Internal samples belong to the prepared plan; backend samples
     * belong to the backend for one callback only. For backend views, channel
     * is zero/ignored and physical numbering follows descriptor span positions.
     * A const descriptor prevents rebinding its pointer, but the pointed-to
     * samples remain writable. Rebinding belongs to the descriptor owner at each
     * callback; Nodes must not retain backend pointers. No allocation/ownership.
     * @pre If consumed with frames > 0, buffer points to at least frames writable
     * floats. No sample may be written for frames == 0. Null is allowed during
     * preparation but not for a descriptor consumed by a positive-frame block.
     */
    struct OutputChannel {
        ChannelID channel{}; ///< Logical identity; zero/ignored for backend views.
        float* buffer = nullptr; ///< Borrowed writable samples; never freed here.
    };

    /**
     * @brief Check non-null pointers for the input descriptors to be consumed.
     * @param consumed Valid descriptor storage selected by the caller for use.
     * @param frames Number of samples per channel that the caller will consume.
     * @return ErrorType::NullBuffer if frames > 0 and any buffer is null;
     * success for zero frames or no consumed descriptors.
     * @warning Only pointer presence is checked, not sample capacity, lifetime
     * or channel identity. The caller still guarantees at least frames live
     * floats per consumed buffer. This function never reads sample memory.
     * @note Pure constexpr/noexcept control-side validation without allocation,
     * locks or I/O. No Node call or runtime context binding is performed here.
     */
    [[nodiscard]] constexpr Result
    validateChannelBuffers(const std::span<const InputChannel> consumed,
                           const FrameCount frames) noexcept {
        if (frames != 0) {
            for (const auto& channel : consumed) {
                if (channel.buffer == nullptr) {
                    return std::unexpected(ErrorType::NullBuffer);
                }
            }
        }
        return {};
    }

    /**
     * @brief Check non-null pointers for the output descriptors to be consumed.
     * @return ErrorType::NullBuffer if frames > 0 and any buffer is null;
     * success for zero frames or no consumed descriptors.
     * @warning Validates pointer presence only: capacity and lifetime of writable
     * sample buffers remain caller preconditions. Samples are never accessed.
     * @note Pure constexpr/noexcept control-side operation without allocation,
     * locks or I/O. See the InputChannel overload for the complete contract.
     */
    [[nodiscard]] constexpr Result
    validateChannelBuffers(const std::span<const OutputChannel> consumed,
                           const FrameCount frames) noexcept {
        if (frames != 0) {
            for (const auto& channel : consumed) {
                if (channel.buffer == nullptr) {
                    return std::unexpected(ErrorType::NullBuffer);
                }
            }
        }
        return {};
    }

    struct Channel {
        std::uint64_t nodeID;
        std::uint64_t channel;
        PortType port;
        
        bool operator==(const Channel&) const = default;
    };
}
