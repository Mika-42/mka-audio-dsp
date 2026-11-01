module;
#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
export module mka.audio.dsp.execution_plan;
export import mka.audio.dsp.node;
export import mka.audio.dsp.connection;
export import mka.audio.dsp.compile_config;
export import mka.audio.process_context;
export import mka.audio.dsp.signal_liveness;

export namespace mka::audio::dsp {
    /// Sticky, numeric callback diagnostics; formatting/handling belongs to control.
    enum class CallbackDiagnostic : std::uint32_t {
        None=0, PlanNotPrepared=1U, FrameCapacityExceeded=2U,
        ChannelCapacityExceeded=4U, NullBuffer=8U, BufferCapacityExceeded=16U,
        InvalidContext=32U
    };
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
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

    /** Explicit mono delay on a logical edge, supplied by control-side PDC analysis. */
    struct ConnectionDelay {
        Connection connection;
        std::uint64_t samples = 0;
    };

    /** Control-side maximum callback shape; counts are physical, not ChannelIDs. */
    struct BackendConfig {
        FrameCount maxFrames = 0;
        std::size_t inputChannels = 0;
        std::size_t outputChannels = 0;
        /// All retained plan payload + alignment allowance; excludes allocator overhead/temporaries.
        std::size_t memoryBudgetBytes = std::numeric_limits<std::size_t>::max();
        SlotStrategy slotStrategy = SlotStrategy::Reuse;
        std::uint32_t maxBufferSlots = std::numeric_limits<std::uint32_t>::max();
        /// Borrowed only during prepare; no Config span is retained in the prepared state.
        std::span<const ConnectionDelay> connectionDelays;
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
    class ExecutionPlan final {
        static constexpr auto noBuffer = std::numeric_limits<std::size_t>::max();
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
            BufferPool pool;
            std::vector<AudioTask> tasks;
            std::vector<InputChannel> backendInputs;
            std::vector<OutputChannel> backendOutputs;

            struct Operation {
                OperationKind kind;
                std::size_t index;
            };
            struct DelayTask {
                std::size_t inputSlot = noBuffer, outputSlot = noBuffer;
                std::size_t samples = 0, position = 0;
                std::vector<float> history;
                const float* input = nullptr;
                float* output = nullptr;

                void process(const FrameCount frames) noexcept {
                    if (samples == 0) {
                        if (frames) std::copy_n(input, frames, output);
                        return;
                    }
                    for (FrameCount f = 0; f < frames; ++f) {
                        output[f] = history[position];
                        history[position] = input[f];
                        if (++position == samples) position = 0;
                    }
                }
            };
            std::vector<Operation> operations;
            std::vector<DelayTask> delays;
            std::vector<SignalLifetime> lifetimes;
            SlotAssignment assignment;
            PlanMemoryUsage memory;

            void rebindInternal() noexcept {
                for (auto& task : tasks) {
                    for (std::size_t i = 0; i < task.inputs.size(); ++i)
                        task.inputs[i].buffer = task.inputSlots[i] == noBuffer ? pool.silence() :
                            pool.buffer(static_cast<BufferId>(task.inputSlots[i]));
                    for (std::size_t i = 0; i < task.auxiliary.size(); ++i)
                        task.auxiliary[i].buffer = task.auxiliarySlots[i] == noBuffer ? pool.silence() :
                            pool.buffer(static_cast<BufferId>(task.auxiliarySlots[i]));
                    for (std::size_t i = 0; i < task.outputs.size(); ++i)
                        task.outputs[i].buffer = pool.buffer(static_cast<BufferId>(task.outputSlots[i]));
                }
                for (auto& delay : delays) {
                    delay.input = pool.buffer(static_cast<BufferId>(delay.inputSlot));
                    delay.output = pool.buffer(static_cast<BufferId>(delay.outputSlot));
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
              audio_(other.audio_ ? std::make_unique<AudioState>(*other.audio_) : nullptr),
              diagnostics_(other.diagnostics_.load(std::memory_order_relaxed)) {
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
              outputTasks_(std::move(other.outputTasks_)), audio_(std::move(other.audio_)),
              diagnostics_(other.diagnostics_.exchange(0,std::memory_order_relaxed)) {
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
        /// Immutable control-side diagnostics; these tables are never consulted in audio.
        [[nodiscard]] std::span<const SignalLifetime> signalLifetimes() const noexcept {
            return audio_ ? std::span<const SignalLifetime>{audio_->lifetimes} : std::span<const SignalLifetime>{};
        }
        [[nodiscard]] std::span<const BufferId> signalSlots() const noexcept {
            return audio_ ? std::span<const BufferId>{audio_->assignment.slotOf} : std::span<const BufferId>{};
        }
        [[nodiscard]] std::uint32_t bufferSlotCount() const noexcept {
            return audio_ ? audio_->pool.slotCapacity() : 0;
        }
        [[nodiscard]] PlanMemoryUsage preparedMemoryUsage() const noexcept {
            return audio_ ? audio_->memory : PlanMemoryUsage{};
        }

        /** Borrowed membership of this generation, including desired-graph removals. */
        [[nodiscard]] bool references(const NodeID id) const noexcept {
            return taskIndexOf(id).has_value();
        }

        /**
         * @brief Canonical synchronous audio entry point; no allocation or lock.
         * @pre This generation is prepared, active, single-threaded and not
         * concurrently copied, moved, prepared or retired. The backend owns io.
         * Publication/lifecycle synchronization belongs to the callback owner.
         */
        void exec(const mka::audio::AudioProcessContext& io) noexcept {
            (void)execute(io.inputs.size(), io.outputs.size(), io.frames,
                [&](const std::size_t i) noexcept { return io.inputs[i]; },
                [&](const std::size_t i) noexcept { return io.outputs[i]; },
                io.inputCapacityFrames,io.outputCapacityFrames);
        }

        /** Atomic numeric snapshot; no synchronization of plan lifetime or samples. */
        [[nodiscard]] std::uint32_t callbackDiagnostics() const noexcept {
            return diagnostics_.load(std::memory_order_relaxed);
        }
        /** Drain sticky bits, including safely from control during audio execution. */
        [[nodiscard]] std::uint32_t takeCallbackDiagnostics() noexcept {
            return diagnostics_.exchange(0,std::memory_order_relaxed);
        }

        /**
         * @brief Prepare owned descriptors/storage on control, without editing Nodes.
         * @details Logical edges and roles are snapshotted. Capacity/semantic
         * and resource errors return expected, without publishing partial state.
         * Finalize Node/Delay order, prove lifetimes, assign slots, check total
         * budget, prepare samples/history, then bind in stable heap storage.
         * Failure leaves existing prepared state unchanged. No publication here.
         */
        [[nodiscard]] Result prepareBackend(const BackendConfig& config,
            const std::span<const Connection> connections,
            const std::span<const NodeID> outputNodes) try {
            auto state = std::make_unique<AudioState>();
            state->config = config;
            state->config.connectionDelays = {};
            if (config.inputChannels > state->backendInputs.max_size() ||
                config.outputChannels > state->backendOutputs.max_size()) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded});
            }
            std::unordered_map<NodeID, std::size_t> positions;
            positions.reserve(plan_.size());
            state->tasks.resize(plan_.size());
            std::size_t signalCount = 0;
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                if (!positions.emplace(plan_[i]->id(), i).second)
                    return std::unexpected(Error{ErrorCode::InvalidParameter, plan_[i]->id()});
                const auto& cfg = plan_[i]->config();
                if (const auto result = validateConfig(cfg); !result) {
                    auto error = result.error();
                    error.node = plan_[i]->id();
                    return std::unexpected(error);
                }
                if (cfg.layoutOutput.size() > std::numeric_limits<std::size_t>::max() - signalCount)
                    return std::unexpected(Error{ErrorCode::CapacityExceeded, plan_[i]->id()});
                signalCount += cfg.layoutOutput.size();
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
                const auto [existing,inserted]=incoming.emplace(connection.to,connection.from);
                if (!inserted)
                    return std::unexpected(Error{existing->second == connection.from ?
                        ErrorCode::ConnectionExists : ErrorCode::InputOccupied,
                        decodeNode(connection.to),connection.to});
                state->tasks[to->second].isSource = false;
            }
            std::unordered_map<ChannelID, std::size_t> outputSlots;
            outputSlots.reserve(signalCount);
            std::size_t slot = 0;
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                auto& task = state->tasks[i];
                task.outputs.reserve(plan_[i]->config().layoutOutput.size());
                task.outputSlots.reserve(plan_[i]->config().layoutOutput.size());
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
                    // Final descriptor and binding sizes are fixed on control,
                    // before any sample pointer or ProcessContext span is patched.
                    descriptors.reserve(layout.size());
                    bindings.reserve(layout.size());
                    for (const auto index : layout) {
                        const auto channel = encode(plan_[i]->id(), port, index);
                        const auto edge = incoming.find(channel);
                        const auto source = edge == incoming.end() ? outputSlots.end() : outputSlots.find(edge->second);
                        // Missing/unproduced logical channels read dedicated silence.
                        descriptors.push_back(InputChannel{channel, nullptr});
                        bindings.push_back(source == outputSlots.end() ? noBuffer : source->second);
                    }
                };
                if (!task.isSource)
                    bindInputs(plan_[i]->config().layoutInput, PortType::In, task.inputs, task.inputSlots);
                bindInputs(plan_[i]->config().layoutAuxiliary, PortType::Aux, task.auxiliary, task.auxiliarySlots);
            }
            std::unordered_map<ChannelID, std::uint64_t> requestedDelays;
            for (const auto& delay : config.connectionDelays) {
                const auto edge = incoming.find(delay.connection.to);
                if (edge == incoming.end() || edge->second != delay.connection.from ||
                    !requestedDelays.emplace(delay.connection.to, delay.samples).second)
                    return std::unexpected(Error{ErrorCode::InvalidParameter});
                if (delay.samples > std::numeric_limits<std::size_t>::max())
                    return std::unexpected(Error{ErrorCode::ArithmeticOverflow});
                if (const auto bytes = checkedSizeMultiply(delay.samples, sizeof(float)); !bytes)
                    return std::unexpected(bytes.error());
                if (delay.samples > std::vector<float>{}.max_size())
                    return std::unexpected(Error{ErrorCode::CapacityExceeded});
            }
            // Establish the COMPLETE Node+Delay sequence before computing liveness.
            std::vector<SignalOperation> signals;
            for (std::size_t i = 0; i < state->tasks.size(); ++i) {
                auto& task = state->tasks[i];
                const auto insertDelays = [&](const auto& descriptors, auto& bindings) {
                    for (std::size_t c = 0; c < descriptors.size(); ++c) {
                        const auto found = requestedDelays.find(descriptors[c].channel);
                        // Absent source/destination layouts remain silent, never index an unprepared slot.
                        if (found == requestedDelays.end() || bindings[c] == noBuffer) continue;
                        if (signalCount == std::numeric_limits<std::size_t>::max())
                            throw std::length_error("signal capacity");
                        const auto input = bindings[c], output = signalCount++;
                        typename AudioState::DelayTask delay;
                        delay.inputSlot = input;
                        delay.outputSlot = output;
                        delay.samples = static_cast<std::size_t>(found->second);
                        state->operations.push_back({OperationKind::Delay, state->delays.size()});
                        state->delays.push_back(std::move(delay));
                        signals.push_back({OperationKind::Delay, {input}, {output}});
                        bindings[c] = output;
                    }
                };
                insertDelays(task.inputs, task.inputSlots);
                insertDelays(task.auxiliary, task.auxiliarySlots);
                SignalOperation operation{OperationKind::Node, {}, task.outputSlots};
                for (const auto s : task.inputSlots) if (s != noBuffer) operation.reads.push_back(s);
                for (const auto s : task.auxiliarySlots) if (s != noBuffer) operation.reads.push_back(s);
                signals.push_back(std::move(operation));
                state->operations.push_back({OperationKind::Node, i});
            }
            auto lifetimes = calculateLifetimes(signals, signalCount);
            if (!lifetimes) return std::unexpected(lifetimes.error());
            auto assignment = assignBufferSlots(*lifetimes, signals.size(), config.slotStrategy, config.maxBufferSlots);
            if (!assignment) return std::unexpected(assignment.error());
            state->lifetimes = std::move(*lifetimes);
            state->assignment = std::move(*assignment);
            // Replace all compile-only SignalIDs by validated physical BufferIds.
            for (auto& task : state->tasks) {
                for (auto& s : task.inputSlots) if (s != noBuffer) s = state->assignment.slotOf[s];
                for (auto& s : task.auxiliarySlots) if (s != noBuffer) s = state->assignment.slotOf[s];
                for (auto& s : task.outputSlots) s = state->assignment.slotOf[s];
            }
            std::vector<std::uint64_t> histories;
            histories.reserve(state->delays.size());
            for (auto& delay : state->delays) {
                delay.inputSlot = state->assignment.slotOf[delay.inputSlot];
                delay.outputSlot = state->assignment.slotOf[delay.outputSlot];
                histories.push_back(delay.samples);
            }
            // Count actual retained array capacities + their owner objects. This
            // also includes structural plan metadata, not the borrowed Nodes.
            std::vector<MemoryArray> metadata;
            const auto account = [&](const auto& values) {
                using T = typename std::remove_cvref_t<decltype(values)>::value_type;
                metadata.push_back({values.capacity(), sizeof(T), alignof(T)});
            };
            metadata.push_back({1, sizeof(ExecutionPlan), alignof(ExecutionPlan)});
            metadata.push_back({1, sizeof(AudioState), alignof(AudioState)});
            account(plan_); account(layerIndices_); account(borrows_); account(taskInfos_);
            account(executionTasks_); account(taskIndices_); account(sourceTasks_); account(outputTasks_);
            account(state->tasks); account(state->operations); account(state->delays);
            account(state->lifetimes); account(state->assignment.slotOf);
            metadata.push_back({config.inputChannels, sizeof(InputChannel), alignof(InputChannel)});
            metadata.push_back({config.outputChannels, sizeof(OutputChannel), alignof(OutputChannel)});
            for (const auto& task : state->tasks) {
                account(task.inputs); account(task.auxiliary); account(task.outputs);
                account(task.inputSlots); account(task.auxiliarySlots); account(task.outputSlots);
            }
            const auto memory = measurePlanMemory({state->assignment.peakSlots, config.maxFrames, histories, metadata},
                                                  config.memoryBudgetBytes);
            if (!memory) return std::unexpected(memory.error());
            state->memory = *memory;
            if (const auto prepared = state->pool.prepare(state->assignment.peakSlots, config.maxFrames); !prepared)
                return std::unexpected(prepared.error());
            for (auto& delay : state->delays) delay.history.resize(delay.samples, 0.F);
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
            diagnostics_.store(0,std::memory_order_relaxed);
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::PrepareFailed});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        }

        /**
         * @brief Execute one backend callback with no allocations or ownership changes.
         * @details Zero every physical output once before Nodes run. All marked
         * writers see the same physical views, in stable producer-first order;
         * the last effective sample write wins, with no implicit mixing.
         * Frame/count/null rejection silences the known writable external range
         * and records a lock-free diagnostic, without processing any Node.
         * This legacy adapter bounds rejected output writes to prepared maxFrames;
         * use AudioProcessContext for explicit external sample capacities.
         * Backend descriptor pointers are cleared before returning.
         */
        [[nodiscard]] bool exec(const BackendBlock& block) noexcept {
            return execute(block.inputs.size(), block.outputs.size(), block.frames,
                [&](const std::size_t i) noexcept { return block.inputs[i]; },
                [&](const std::size_t i) noexcept { return block.outputs[i]; },
                block.frames,legacyWritableFrames(block.frames));
        }

        /**
         * @brief Descriptor-view callback adapter: inputs and backendOutputs are physical.
         * @pre auxiliary and outputs are empty (this is not a Node's logical context).
         * Backend ChannelIDs are ignored; the plan sets physical descriptor IDs to zero.
         */
        [[nodiscard]] bool exec(const ProcessContext& callback) noexcept {
            if (!callback.auxiliary.empty() || !callback.outputs.empty())
                return rejectCallback(CallbackDiagnostic::InvalidContext,callback.backendOutputs.size(),
                    callback.frames,[&](const std::size_t i) noexcept { return callback.backendOutputs[i].buffer; },
                    legacyWritableFrames(callback.frames));
            return execute(callback.inputs.size(), callback.backendOutputs.size(), callback.frames,
                [&](const std::size_t i) noexcept { return callback.inputs[i].buffer; },
                [&](const std::size_t i) noexcept { return callback.backendOutputs[i].buffer; },
                callback.frames,legacyWritableFrames(callback.frames));
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

        FrameCount legacyWritableFrames(const FrameCount frames) const noexcept {
            return audio_ ? std::min(frames,audio_->config.maxFrames) : 0;
        }

        template<typename OutputAt>
        bool rejectCallback(const CallbackDiagnostic diagnostic, const std::size_t outputCount,
            const FrameCount frames, OutputAt outputAt, const FrameCount outputCapacity) noexcept {
            diagnostics_.fetch_or(static_cast<std::uint32_t>(diagnostic),std::memory_order_relaxed);
            const auto writable=std::min(frames,outputCapacity);
            if (writable != 0)
                for (std::size_t i=0;i<outputCount;++i)
                    if (auto* const output=outputAt(i)) std::fill_n(output,writable,0.F);
            return false;
        }

        template<typename InputAt, typename OutputAt>
        bool execute(const std::size_t inputCount, const std::size_t outputCount,
                     const FrameCount frames, InputAt inputAt, OutputAt outputAt,
                     const FrameCount inputCapacity, const FrameCount outputCapacity) noexcept {
            if (!audio_)
                return rejectCallback(CallbackDiagnostic::PlanNotPrepared,outputCount,frames,outputAt,outputCapacity);
            if (frames > audio_->config.maxFrames)
                return rejectCallback(CallbackDiagnostic::FrameCapacityExceeded,outputCount,frames,outputAt,outputCapacity);
            if (inputCount > audio_->config.inputChannels || outputCount > audio_->config.outputChannels)
                return rejectCallback(CallbackDiagnostic::ChannelCapacityExceeded,outputCount,frames,outputAt,outputCapacity);
            if ((inputCount != 0 && frames > inputCapacity) || (outputCount != 0 && frames > outputCapacity))
                return rejectCallback(CallbackDiagnostic::BufferCapacityExceeded,outputCount,frames,outputAt,outputCapacity);
            if (frames != 0) {
                for (std::size_t i = 0; i < inputCount; ++i)
                    if (inputAt(i) == nullptr)
                        return rejectCallback(CallbackDiagnostic::NullBuffer,outputCount,frames,outputAt,outputCapacity);
                for (std::size_t i = 0; i < outputCount; ++i)
                    if (outputAt(i) == nullptr)
                        return rejectCallback(CallbackDiagnostic::NullBuffer,outputCount,frames,outputAt,outputCapacity);
            }
            auto& state = *audio_;
            for (std::size_t i = 0; i < inputCount; ++i) state.backendInputs[i] = {0, inputAt(i)};
            for (std::size_t i = 0; i < outputCount; ++i) {
                state.backendOutputs[i] = {0, outputAt(i)};
                if (frames != 0) std::fill_n(outputAt(i), frames, 0.F);
            }
            const auto physicalInputs = std::span<const InputChannel>{state.backendInputs}.first(inputCount);
            const auto physicalOutputs = std::span<const OutputChannel>{state.backendOutputs}.first(outputCount);
            for (const auto& compiled : state.operations) {
                if (compiled.kind == OperationKind::Delay) {
                    state.delays[compiled.index].process(frames);
                    continue;
                }
                const auto i = compiled.index;
                auto& operation = executionTasks_[i];
                if (state.tasks[i].isSource) operation.context.inputs = physicalInputs;
                if (state.tasks[i].isOutput) operation.context.backendOutputs = physicalOutputs;
                operation.context.frames = frames;
                // Clear only this operation's assigned logical destinations,
                // immediately before it runs. Inputs, live fan-out, silence and
                // physical outputs are never touched by this per-Node clearing.
                if (frames != 0)
                    for (const auto& output : operation.context.outputs)
                        std::fill_n(output.buffer,frames,0.F);
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
            const auto diagnostic=diagnostics_.load(std::memory_order_relaxed);
            diagnostics_.store(other.diagnostics_.load(std::memory_order_relaxed),std::memory_order_relaxed);
            other.diagnostics_.store(diagnostic,std::memory_order_relaxed);
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
        std::atomic<std::uint32_t> diagnostics_{0};
    };
}
