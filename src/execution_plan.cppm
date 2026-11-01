module;
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
export module mka.audio.dsp.execution_plan;
export import mka.audio.dsp.node;
export import mka.audio.dsp.connection;
export import mka.audio.dsp.compile_config;

export namespace mka::audio::dsp {
    /**
     * @brief One immutable structural task, in the plan's topological order.
     * @details The pointer borrows a stable Node owned by the graph. Source
     * means no logical incoming In/Aux edge, including undeclared channels.
     * Output membership is independent of successors and of source status.
     * Compilation never invokes Node::process or prepares audio storage.
     */
    struct TaskInfo {
        Node* node = nullptr;
        NodeID id = invalidNodeID;
        std::size_t index = 0;
        bool isSource = true;
        bool isOutput = false;
    };

    /**
     * @brief One concrete, non-owning processing operation.
     * @details Context descriptors and internal samples belong to the plan.
     * Backend views are rebound for one callback and cleared before returning.
     * The graph owns node and must outlive every plan; remove defers destruction
     * while any generation borrows it. No inheritance, allocation or ownership.
     * @pre node is non-null in every prepared task. Only the active, single
     * audio thread may update context frames and external views during exec.
     */
    struct ExecutionTask {
        Node* node = nullptr;
        ProcessContext context{};
    };

    /** Control-side maximum callback shape; counts are physical, not ChannelIDs. */
    struct BackendConfig {
        FrameCount maxFrames = 0;
        std::size_t inputChannels = 0;
        std::size_t outputChannels = 0;
    };

    /**
     * @brief Borrowed driver buffers valid for this callback only.
     * @pre Non-null pointers in an accepted block cover frames samples. Input memory must not
     * overlap writable output/plan storage. The caller validates wide sizes
     * before narrowing to FrameCount. No pointer is retained after exec returns.
     */
    struct BackendBlock {
        std::span<const float* const> inputs;
        std::span<float* const> outputs;
        FrameCount frames = 0;
    };
}

export namespace mka::audio::dsp::detail {
    /// Compatibility alias: DAG storage and preparation use the same value type.
    using PlanConnection = Connection;
}

export namespace mka::audio::dsp {
    /**
     * @brief Control-owned plan borrowing stable Node addresses.
     * @details Copy/move/prepare/retire only on control after quiescence. The DAG
     * must outlive every plan and borrowed task/layer view. Audio execution is
     * single-threaded and non-reentrant; copied plans own separate buffers but
     * still share Node DSP state and must not process it concurrently.
     * Standard C++ construction/copy may throw resource exceptions on control;
     * expected-returning preparation/compilation boundaries translate them to Error.
     */
    class ExecutionPlan {
        struct AudioTask {
            std::vector<InputChannel> inputs;
            std::vector<InputChannel> auxiliary;
            std::vector<OutputChannel> outputs;
            std::vector<std::size_t> inputSlots;
            std::vector<std::size_t> auxiliarySlots;
            std::vector<std::size_t> outputSlots;
            bool isSource = true;
            bool isOutput = false;
        };

        struct AudioState {
            BackendConfig config;
            // Slot zero is read-only silence. Outputs each get their own slot.
            std::vector<float> samples;
            std::vector<AudioTask> tasks;
            std::vector<InputChannel> backendInputs;
            std::vector<OutputChannel> backendOutputs;

            float* buffer(const std::size_t slot) noexcept {
                return config.maxFrames == 0 ? nullptr :
                    samples.data() + slot * static_cast<std::size_t>(config.maxFrames);
            }
            void rebindInternal() noexcept {
                for (auto& task : tasks) {
                    for (std::size_t i = 0; i < task.inputs.size(); ++i)
                        task.inputs[i].buffer = buffer(task.inputSlots[i]);
                    for (std::size_t i = 0; i < task.auxiliary.size(); ++i)
                        task.auxiliary[i].buffer = buffer(task.auxiliarySlots[i]);
                    for (std::size_t i = 0; i < task.outputs.size(); ++i)
                        task.outputs[i].buffer = buffer(task.outputSlots[i]);
                }
            }
            void clearBackend(const std::size_t inputs, const std::size_t outputs) noexcept {
                for (std::size_t i = 0; i < inputs; ++i) backendInputs[i].buffer = nullptr;
                for (std::size_t i = 0; i < outputs; ++i) backendOutputs[i].buffer = nullptr;
            }
        };

    public:
        /// Empty, unprepared target; constructing it performs no allocation.
        ExecutionPlan() noexcept = default;

        /**
         * @brief Construct validated, topologically ordered tasks on control.
         * @pre Every task has a non-null, live Node and its matching id; ids
         * are unique and index equals its position. Owners outlive this plan.
         * @note Storage acquisition can throw; compile translates failures.
         */
        explicit ExecutionPlan(std::vector<TaskInfo> tasks)
            : taskInfos_(std::move(tasks)) {
            plan_.reserve(taskInfos_.size());
            layerIndices_.reserve(taskInfos_.size() + 1);
            for (std::size_t i = 0; i < taskInfos_.size(); ++i) {
                const auto& task = taskInfos_[i];
                assert(task.node != nullptr && task.id == task.node->id() && task.index == i);
                plan_.push_back(task.node);
                layerIndices_.push_back(i);
            }
            layerIndices_.push_back(taskInfos_.size());
            initializeStructuralIndices();
        }

        /** Structural plan; tasks must be non-null and owned longer than the plan. */
        ExecutionPlan(std::vector<Node*> tasks, std::vector<std::size_t> layerIndices)
            : plan_(std::move(tasks)), layerIndices_(std::move(layerIndices)) {
            taskInfos_.reserve(plan_.size());
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                const auto* node = plan_[i];
                assert(node != nullptr);
                taskInfos_.push_back({plan_[i], node->id(), i, true, false});
            }
            initializeStructuralIndices();
        }

        ExecutionPlan(const ExecutionPlan& other)
            : plan_(other.plan_), layerIndices_(other.layerIndices_), borrows_(other.borrows_),
              taskInfos_(other.taskInfos_), executionTasks_(other.executionTasks_), taskIndices_(other.taskIndices_),
              sourceTasks_(other.sourceTasks_), outputTasks_(other.outputTasks_),
              audio_(other.audio_ ? std::make_unique<AudioState>(*other.audio_) : nullptr) {
            // Future preparation refreshes roles without allocating at commit.
            sourceTasks_.reserve(plan_.size());
            outputTasks_.reserve(plan_.size());
            if (audio_) {
                // Raw descriptor pointers must address the copy's owned storage.
                audio_->rebindInternal();
                audio_->clearBackend(audio_->backendInputs.size(), audio_->backendOutputs.size());
                bindTaskContexts();
            }
        }
        ExecutionPlan& operator=(const ExecutionPlan& other) {
            if (this != &other) {
                ExecutionPlan replacement(other);
                swapContents(replacement);
            }
            return *this;
        }
        ExecutionPlan(ExecutionPlan&& other) noexcept
            : plan_(std::move(other.plan_)), layerIndices_(std::move(other.layerIndices_)),
              borrows_(std::move(other.borrows_)), taskInfos_(std::move(other.taskInfos_)),
              executionTasks_(std::move(other.executionTasks_)), taskIndices_(std::move(other.taskIndices_)), sourceTasks_(std::move(other.sourceTasks_)),
              outputTasks_(std::move(other.outputTasks_)), audio_(std::move(other.audio_)) {
            other.plan_.clear();
            other.layerIndices_.clear();
            other.borrows_.clear();
            other.taskInfos_.clear();
            other.executionTasks_.clear();
            other.taskIndices_.clear();
            other.sourceTasks_.clear();
            other.outputTasks_.clear();
        }
        ExecutionPlan& operator=(ExecutionPlan&& other) noexcept {
            if (this != &other) {
                ExecutionPlan replacement(std::move(other));
                swapContents(replacement);
            }
            return *this;
        }
        ~ExecutionPlan() = default; // Release resources/borrow tokens on control.

        [[nodiscard]] std::span<Node* const> tasks() const noexcept { return plan_; }
        /** Read-only task view, invalidated by control-side replacement/retirement. */
        [[nodiscard]] std::span<const TaskInfo> taskInfos() const noexcept {
            return taskInfos_;
        }
        /// Prepared operations; descriptors remain fixed until control-side replacement.
        [[nodiscard]] std::span<const ExecutionTask> executionTasks() const noexcept {
            return executionTasks_;
        }
        /// Source task positions, in topological order; no ownership transfer.
        [[nodiscard]] std::span<const std::size_t> sourceTasks() const noexcept { return sourceTasks_; }
        /// Output task positions, in topological order; may overlap sourceTasks.
        [[nodiscard]] std::span<const std::size_t> outputTasks() const noexcept { return outputTasks_; }
        /** Allocation-free lookup of a compiled task's position; absent ids return nullopt. */
        [[nodiscard]] std::optional<std::size_t> taskIndexOf(const NodeID id) const noexcept {
            const auto found = std::lower_bound(taskIndices_.begin(), taskIndices_.end(), id,
                [](const auto& entry, const NodeID key) noexcept { return entry.first < key; });
            if (found == taskIndices_.end() || found->first != id) return std::nullopt;
            return found->second;
        }
        [[nodiscard]] std::size_t layerCount() const noexcept {
            return layerIndices_.empty() ? 0 : layerIndices_.size() - 1;
        }
        [[nodiscard]] std::span<Node* const> layer(const std::size_t index) const noexcept {
            assert(index < layerCount());
            return tasks().subspan(layerIndices_[index], layerIndices_[index + 1] - layerIndices_[index]);
        }
        [[nodiscard]] bool isBackendPrepared() const noexcept { return audio_ != nullptr; }

        /**
         * @brief Prepare owned descriptors/storage on control, without editing Nodes.
         * @details Logical edges and roles are snapshotted. Capacity/semantic
         * and resource errors return expected, without publishing partial state.
         * Failure leaves existing prepared state unchanged. No slot recycling.
         */
        [[nodiscard]] Result prepareBackend(const BackendConfig& config,
            const std::span<const Connection> connections,
            const std::span<const NodeID> outputNodes) try {
            auto state = std::make_unique<AudioState>();
            state->config = config;
            if (config.inputChannels > state->backendInputs.max_size() ||
                config.outputChannels > state->backendOutputs.max_size()) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded});
            }
            std::unordered_map<NodeID, std::size_t> positions;
            positions.reserve(plan_.size());
            state->tasks.resize(plan_.size());
            const auto stride = static_cast<std::size_t>(config.maxFrames);
            const auto maxSlots = stride == 0 ? state->samples.max_size() : state->samples.max_size() / stride;
            std::size_t slots = 1; // Silence, even for a graph without outputs.
            if (maxSlots < slots) return std::unexpected(Error{ErrorCode::CapacityExceeded});
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                if (!positions.emplace(plan_[i]->id(), i).second)
                    return std::unexpected(Error{ErrorCode::InvalidParameter, plan_[i]->id()});
                const auto& cfg = plan_[i]->config();
                if (const auto result = validateConfig(cfg); !result) {
                    auto error = result.error();
                    error.node = plan_[i]->id();
                    return std::unexpected(error);
                }
                if (cfg.layoutOutput.size() > maxSlots - slots)
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, plan_[i]->id()});
                slots += cfg.layoutOutput.size();
            }
            for (const auto id : outputNodes) {
                const auto found = positions.find(id);
                if (found == positions.end()) return std::unexpected(Error{ErrorCode::NodeNotFound, id});
                state->tasks[found->second].isOutput = true;
            }
            std::unordered_map<ChannelID, ChannelID> incoming;
            incoming.reserve(connections.size());
            for (const auto& connection : connections) {
                const auto sourcePort = decodePort(connection.from);
                const auto destinationPort = decodePort(connection.to);
                if (!isValidPort(sourcePort) || !isValidPort(destinationPort)) {
                    const auto channel = !isValidPort(sourcePort) ? connection.from : connection.to;
                    return std::unexpected(Error{ErrorCode::InvalidPort, decodeNode(channel), channel});
                }
                if (!isOutputPort(sourcePort) || !isInputPort(destinationPort)) {
                    const auto channel = !isOutputPort(sourcePort) ? connection.from : connection.to;
                    return std::unexpected(Error{ErrorCode::InvalidDirection, decodeNode(channel), channel});
                }
                const auto from = positions.find(decodeNode(connection.from));
                const auto to = positions.find(decodeNode(connection.to));
                if (from == positions.end())
                    return std::unexpected(Error{ErrorCode::NodeNotFound, decodeNode(connection.from), connection.from});
                if (to == positions.end())
                    return std::unexpected(Error{ErrorCode::NodeNotFound, decodeNode(connection.to), connection.to});
                if (from->second >= to->second)
                    return std::unexpected(Error{ErrorCode::InvalidParameter, decodeNode(connection.to), connection.to});
                if (!incoming.emplace(connection.to, connection.from).second)
                    return std::unexpected(Error{ErrorCode::InputOccupied, decodeNode(connection.to), connection.to});
                state->tasks[to->second].isSource = false;
            }
            std::unordered_map<ChannelID, std::size_t> outputSlots;
            outputSlots.reserve(slots - 1);
            std::size_t slot = 1;
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                auto& task = state->tasks[i];
                for (const auto index : plan_[i]->config().layoutOutput) {
                    const auto channel = encode(plan_[i]->id(), PortType::Out, index);
                    outputSlots.emplace(channel, slot);
                    task.outputs.push_back({channel, nullptr});
                    task.outputSlots.push_back(slot++);
                }
            }
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                auto& task = state->tasks[i];
                const auto bindInputs = [&](const auto& layout, const PortType port,
                                            auto& descriptors, auto& bindings) {
                    for (const auto index : layout) {
                        const auto channel = encode(plan_[i]->id(), port, index);
                        const auto edge = incoming.find(channel);
                        const auto source = edge == incoming.end() ? outputSlots.end() : outputSlots.find(edge->second);
                        // Missing/unproduced logical channels read dedicated silence.
                        descriptors.push_back(InputChannel{channel, nullptr});
                        bindings.push_back(source == outputSlots.end() ? std::size_t{0} : source->second);
                    }
                };
                if (!task.isSource)
                    bindInputs(plan_[i]->config().layoutInput, PortType::In, task.inputs, task.inputSlots);
                bindInputs(plan_[i]->config().layoutAuxiliary, PortType::Aux, task.auxiliary, task.auxiliarySlots);
            }
            state->samples.resize(slots * stride, 0.F);
            state->backendInputs.resize(config.inputChannels);
            state->backendOutputs.resize(config.outputChannels);
            state->rebindInternal();
            for (std::size_t i = 0; i < state->tasks.size(); ++i) {
                const auto& task = state->tasks[i];
                const PreparedContextBindings bindings{.inputs = task.inputs, .auxiliary = task.auxiliary,
                    .outputs = task.outputs, .maxFrames = config.maxFrames, .isSource = task.isSource};
                if (const auto result = validateContextBindings(bindings); !result) {
                    auto error = result.error();
                    if (error.node == invalidNodeID) error.node = plan_[i]->id();
                    return std::unexpected(error);
                }
            }
            audio_.swap(state);
            refreshPreparedRoles();
            bindTaskContexts();
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::MemoryBudgetExceeded});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        }

        /**
         * @brief Execute one backend callback with no allocations or ownership changes.
         * @details Zero every physical output once before Nodes run. All marked
         * writers see the same physical views, in stable producer-first order;
         * the last effective sample write wins, with no implicit mixing.
         * Frame/count/null rejection touches no samples; the caller may silence
         * rejected external buffers using its independently known safe capacity.
         * Backend descriptor pointers are cleared before returning.
         */
        [[nodiscard]] bool exec(const BackendBlock& block) noexcept {
            return execute(block.inputs.size(), block.outputs.size(), block.frames,
                [&](const std::size_t i) noexcept { return block.inputs[i]; },
                [&](const std::size_t i) noexcept { return block.outputs[i]; });
        }

        /**
         * @brief Descriptor-view callback adapter: inputs and backendOutputs are physical.
         * @pre auxiliary and outputs are empty (this is not a Node's logical context).
         * Backend ChannelIDs are ignored; the plan sets physical descriptor IDs to zero.
         */
        [[nodiscard]] bool exec(const ProcessContext& callback) noexcept {
            if (!callback.auxiliary.empty() || !callback.outputs.empty()) return false;
            return execute(callback.inputs.size(), callback.backendOutputs.size(), callback.frames,
                [&](const std::size_t i) noexcept { return callback.inputs[i].buffer; },
                [&](const std::size_t i) noexcept { return callback.backendOutputs[i].buffer; });
        }

    private:
        void initializeStructuralIndices() {
            executionTasks_.reserve(plan_.size());
            borrows_.reserve(plan_.size());
            taskIndices_.reserve(plan_.size());
            sourceTasks_.reserve(plan_.size());
            outputTasks_.reserve(plan_.size());
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                executionTasks_.push_back({plan_[i], {}});
                borrows_.push_back(detail::NodeBorrowAccess::retain(*plan_[i]));
                taskIndices_.emplace_back(plan_[i]->id(), i);
                if (taskInfos_[i].isSource) sourceTasks_.push_back(i);
                if (taskInfos_[i].isOutput) outputTasks_.push_back(i);
            }
            std::sort(taskIndices_.begin(), taskIndices_.end(),
                [](const auto& left, const auto& right) noexcept { return left.first < right.first; });
        }

        void refreshPreparedRoles() noexcept {
            assert(audio_ != nullptr);
            assert(sourceTasks_.capacity() >= plan_.size() && outputTasks_.capacity() >= plan_.size());
            sourceTasks_.clear();
            outputTasks_.clear();
            for (std::size_t i = 0; i < taskInfos_.size(); ++i) {
                auto& task = taskInfos_[i];
                task.isSource = audio_->tasks[i].isSource;
                task.isOutput = audio_->tasks[i].isOutput;
                if (task.isSource) sourceTasks_.push_back(i);
                if (task.isOutput) outputTasks_.push_back(i);
            }
        }

        // Called only after all descriptor/sample storage has reached its final address.
        void bindTaskContexts() noexcept {
            for (std::size_t i = 0; i < executionTasks_.size(); ++i) {
                const auto& task = audio_->tasks[i];
                executionTasks_[i].context = ProcessContext{
                    .inputs = task.isSource ? std::span<const InputChannel>{} : task.inputs,
                    .auxiliary = task.auxiliary, .outputs = task.outputs};
            }
        }

        template<typename InputAt, typename OutputAt>
        bool execute(const std::size_t inputCount, const std::size_t outputCount,
                     const FrameCount frames, InputAt inputAt, OutputAt outputAt) noexcept {
            if (!audio_ || frames > audio_->config.maxFrames ||
                inputCount > audio_->config.inputChannels || outputCount > audio_->config.outputChannels)
                return false;
            if (frames != 0) {
                for (std::size_t i = 0; i < inputCount; ++i) if (inputAt(i) == nullptr) return false;
                for (std::size_t i = 0; i < outputCount; ++i) if (outputAt(i) == nullptr) return false;
            }
            auto& state = *audio_;
            for (std::size_t i = 0; i < inputCount; ++i) state.backendInputs[i] = {0, inputAt(i)};
            for (std::size_t i = 0; i < outputCount; ++i) {
                state.backendOutputs[i] = {0, outputAt(i)};
                if (frames != 0) std::fill_n(outputAt(i), frames, 0.F);
            }
            const auto stride = static_cast<std::size_t>(state.config.maxFrames);
            // Silence slot zero is initialized on control and never written in RT.
            if (frames != 0)
                for (std::size_t offset = stride; offset < state.samples.size(); offset += stride)
                    std::fill_n(state.samples.data() + offset, frames, 0.F);
            const auto physicalInputs = std::span<const InputChannel>{state.backendInputs}.first(inputCount);
            const auto physicalOutputs = std::span<const OutputChannel>{state.backendOutputs}.first(outputCount);
            for (std::size_t i = 0; i < executionTasks_.size(); ++i) {
                auto& operation = executionTasks_[i];
                if (state.tasks[i].isSource) operation.context.inputs = physicalInputs;
                if (state.tasks[i].isOutput) operation.context.backendOutputs = physicalOutputs;
                operation.context.frames = frames;
                operation.node->process(operation.context);
            }
            // No callback-only view or pointer survives this synchronous execution.
            for (std::size_t i = 0; i < executionTasks_.size(); ++i) {
                if (state.tasks[i].isSource) executionTasks_[i].context.inputs = {};
                executionTasks_[i].context.backendOutputs = {};
                executionTasks_[i].context.frames = 0;
            }
            state.clearBackend(inputCount, outputCount);
            return true;
        }

        void swapContents(ExecutionPlan& other) noexcept {
            plan_.swap(other.plan_);
            layerIndices_.swap(other.layerIndices_);
            borrows_.swap(other.borrows_);
            taskInfos_.swap(other.taskInfos_);
            executionTasks_.swap(other.executionTasks_);
            taskIndices_.swap(other.taskIndices_);
            sourceTasks_.swap(other.sourceTasks_);
            outputTasks_.swap(other.outputTasks_);
            audio_.swap(other.audio_);
        }
        std::vector<Node*> plan_;
        std::vector<std::size_t> layerIndices_;
        std::vector<detail::NodeBorrowToken> borrows_;
        std::vector<TaskInfo> taskInfos_;
        std::vector<ExecutionTask> executionTasks_;
        std::vector<std::pair<NodeID, std::size_t>> taskIndices_;
        std::vector<std::size_t> sourceTasks_;
        std::vector<std::size_t> outputTasks_;
        std::unique_ptr<AudioState> audio_;
    };
}
