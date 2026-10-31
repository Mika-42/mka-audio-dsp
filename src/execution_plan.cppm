//
// Created by mika on 10/18/26.
//
module;
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>
export module mka.audio.dsp.execution_plan;
import mka.audio.dsp.node;

export namespace mka::audio::dsp {
    struct ProcessContext;

    class ExecutionPlan {
        public:
        ExecutionPlan(
            std::vector<Node*> tasks,
            std::vector<std::size_t> layerIndices
        )
            : plan_(std::move(tasks)),
              layerIndices_(std::move(layerIndices)) {
        }

        [[nodiscard]]
              std::span<Node* const> tasks() const noexcept {
            return plan_;
        }

        [[nodiscard]]
        std::size_t layerCount() const noexcept {
            return layerIndices_.empty()
                ? 0
                : layerIndices_.size() - 1;
        }

        [[nodiscard]]
        std::span<Node* const> layer(
            const std::size_t index
        ) const noexcept {
            assert(index < layerCount());

            const auto begin = layerIndices_[index];
            const auto end = layerIndices_[index + 1];

            return tasks().subspan(begin, end - begin);
        }

        void exec(const ProcessContext& ctx) noexcept {

        }
        
    private:
        std::vector<Node*> plan_;
        std::vector<std::size_t> layerIndices_ {};
    };
}