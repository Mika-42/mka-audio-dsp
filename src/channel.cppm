//
// Created by mika on 10/16/26.
//
module;
#include <cstdint>

export module mka.audio.dsp.channel;

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

    struct Channel {
        std::uint64_t nodeID;
        std::uint64_t channel;
        PortType port;
        
        bool operator==(const Channel&) const = default;
    };
}
