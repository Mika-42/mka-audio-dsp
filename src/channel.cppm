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

    enum class PortType : std::uint8_t { In, Out, Aux };

    struct Channel {
        std::uint64_t nodeID;
        std::uint64_t channel;
        PortType port;
        
        bool operator==(const Channel&) const = default;
    };
}
