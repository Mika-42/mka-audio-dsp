//
// Created by mika on 10/18/26.
//
module;
#include <cassert>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
export module mka.audio.dsp.execution_plan;
import mka.audio.dsp.node;
import mka.audio.dsp.process_context;

export namespace mka::audio::dsp {
    /**
     * @brief Control-owned plan borrowing stable Node addresses.
     * @details Tasks remain raw, non-owning pointers. Opaque borrow tokens keep
     * DAG::remove from destroying a referenced Node; they do not own the Node.
     * Construct, copy, move and retire plans on control only, after any callback
     * using a retired plan has completed. The DAG/other Node owner must outlive
     * every plan and any use of tasks()/layer() views.
     */
    class ExecutionPlan {
        public:
        ExecutionPlan(
            std::vector<Node*> tasks,
            std::vector<std::size_t> layerIndices
        )
            : plan_(std::move(tasks)),
              layerIndices_(std::move(layerIndices)) {
            borrows_.reserve(plan_.size());
            for (const auto* node : plan_) {
                assert(node != nullptr);
                borrows_.push_back(detail::NodeBorrowAccess::retain(*node));
            }
        }

        ExecutionPlan(const ExecutionPlan&) = default;

        ExecutionPlan& operator=(const ExecutionPlan& other) {
            if (this != &other) {
                ExecutionPlan replacement(other);
                swapContents(replacement);
            }
            return *this;
        }

        ExecutionPlan(ExecutionPlan&& other) noexcept
            : plan_(std::move(other.plan_)),
              layerIndices_(std::move(other.layerIndices_)),
              borrows_(std::move(other.borrows_)) {
            other.plan_.clear();
            other.layerIndices_.clear();
            other.borrows_.clear();
        }

        ExecutionPlan& operator=(ExecutionPlan&& other) noexcept {
            if (this != &other) {
                ExecutionPlan replacement(std::move(other));
                swapContents(replacement);
            }
            return *this;
        }

        /// Release borrow tokens on control; Node ownership remains with the DAG.
        ~ExecutionPlan() = default;

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
        void swapContents(ExecutionPlan& other) noexcept {
            plan_.swap(other.plan_);
            layerIndices_.swap(other.layerIndices_);
            borrows_.swap(other.borrows_);
        }

        std::vector<Node*> plan_;
        std::vector<std::size_t> layerIndices_ {};
        std::vector<detail::NodeBorrowToken> borrows_;
    };
}
