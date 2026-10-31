module;

#include <cstddef>
#include <span>

export module mka.audio.dsp.process_context;

export namespace mka::audio::dsp {

    struct ProcessContext {
        // Un pointeur par canal ; échantillons accessibles en lecture.
        std::span<const float* const> inputs;
        std::span<const float* const> auxiliary;

        // Un pointeur par canal ; échantillons accessibles en écriture.
        std::span<float* const> outputs;

        // Nombre d'échantillons à traiter par canal.
        std::size_t frames = 0;
    };

}