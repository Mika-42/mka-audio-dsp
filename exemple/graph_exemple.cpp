/**
 * @file graph_exemple.cpp
 * @brief Petit mixeur stéréo hors ligne, de la création du DAG à son arrêt.
 *
 *                   +--------------- In -----------+
 * Source -> Gain ---+                              Mix -> Routeur -> Casque
 *                   +-> Effet à latence -> Aux -----+
 *
 * Le routeur écrit les voies physiques 0/1 (casque) et 2/3 (enceintes).
 * Son successeur Casque corrige ensuite 0/1 : Last Writer Wins, sans altérer
 * les sorties logiques du routeur ni les enceintes. Les voies 4/5 sont muettes.
 * Le Mix prévoit aussi un Aux mono optionnel, non connecté, fourni au silence.
 *
 * Contrôle : construction, connexions, préparation DSP/plan, publication,
 * diagnostics et destruction. Audio : uniquement exec() et process() noexcept.
 * Les appels sont sérialisés ici pour rester lisibles et reproductibles.
 * Un vrai backend appelle exec() sur un seul thread audio et doit être stoppé
 * ET joint avant stop/shutdown/destruction ; un simple flag ne suffit pas.
 *
 * Construction : cmake ... -DMKA_BUILD_EXAMPLES=ON
 * Exécution : ./build/debug/mka-graph-example (aucun matériel requis).
 */

// Tous les headers standard précèdent les imports, y compris avec GCC/modules.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import mka.audio.dsp.dag;
import mka.audio.dsp.delay_line;
import mka.audio.dsp.diagnostics;

namespace {
    using namespace mka::audio::dsp;
    constexpr FrameCount maxFrames = 128;
    constexpr ChannelIndex stereoChannels = 2;

    // Adaptation propre à l'exemple, SUR CONTRÔLE seulement. L'API retourne
    // expected<T,Error> ; cette aide transforme un échec fatal en erreur de main.
    template<class T>
    T checked(std::expected<T, Error> result, std::string_view operation) {
        if (!result)
            throw std::runtime_error(std::string(operation) + " : " + formatError(result.error()));
        if constexpr (!std::is_void_v<T>) return std::move(*result);
    }

    void expectRefusal(const Result& result, ErrorCode code, std::string_view operation) {
        if (result || result.error().code != code)
            throw std::runtime_error(std::string(operation) + " : refus attendu absent");
        std::cout << "Refus attendu — " << operation << " : " << formatError(result.error()) << '\n';
    }

    Config stereoLayout() {
        return {.layoutInput = {0, 1}, .layoutOutput = {0, 1}};
    }

    /// Une source topologique reçoit les inputs PHYSIQUES, même sans layoutInput.
    class StereoSource final : public Node {
    public:
        explicit StereoSource(NodeID id) : Node(id, {.layoutOutput = {0, 1}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c) {
                const float* input = ctx.inputs[c].buffer; // Emprunt const, jamais conservé.
                std::copy_n(input, ctx.frames, ctx.outputs[c].buffer);
            }
        }
    };

    class StereoGain final : public Node {
    public:
        StereoGain(NodeID id, float gain) : Node(id, stereoLayout()), gain_(gain) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c)
                for (FrameCount f = 0; f < ctx.frames; ++f)
                    ctx.outputs[c].buffer[f] = gain_ * ctx.inputs[c].buffer[f];
        }
    private:
        const float gain_; // Immuable tant que A ou B emprunte ce Node.
    };

    /// Modèle minimal d'un effet/plugin à latence : le retard déclaré est réel.
    class LatentEffect final : public Node {
    public:
        LatentEffect(NodeID id, LatencySamples latency)
            : Node(id, stereoLayout()), latency_(latency) {
            // Le constructeur tourne sur contrôle, AVANT toute publication.
            // Node n'impose pas de méthode prepare() virtuelle universelle.
            for (auto& line : lines_) checked(line.prepare(latency), "Historique DSP");
        }
        LatencySamples latencySamples() const noexcept override { return latency_; }
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c)
                lines_[c].process(ctx.inputs[c].buffer, ctx.outputs[c].buffer, ctx.frames);
        }
    private:
        const LatencySamples latency_;
        std::array<DelayLine, stereoChannels> lines_; // Propres au DSP, distinctes du PDC.
    };

    class StereoMix final : public Node {
    public:
        explicit StereoMix(NodeID id)
            : Node(id, {.layoutInput = {0, 1}, .layoutAuxiliary = {0, 1, 2}, .layoutOutput = {0, 1}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c)
                for (FrameCount f = 0; f < ctx.frames; ++f)
                    ctx.outputs[c].buffer[f] = ctx.inputs[c].buffer[f]
                        + ctx.auxiliary[c].buffer[f] + ctx.auxiliary[2].buffer[f];
            // Les descriptors suivent l'ordre du Config. Aux 2 non branché vaut
            // zéro ; aucun null-check ni buffer de secours à allouer en audio.
        }
    };

    /// Marqué output, mais reste aussi un producteur LOGIQUE pour Casque.
    class OutputRouter final : public Node {
    public:
        explicit OutputRouter(NodeID id) : Node(id, stereoLayout()) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c)
                for (FrameCount f = 0; f < ctx.frames; ++f) {
                    const float sample = ctx.inputs[c].buffer[f];
                    ctx.outputs[c].buffer[f] = sample; // BufferPool, pas hardware.
                    ctx.backendOutputs[c].buffer[f] = sample;     // Casque 0/1.
                    ctx.backendOutputs[c + 2].buffer[f] = sample; // Enceintes 2/3.
                }
            // Tous les outputs physiques sont accessibles. Aucun ChannelID ni
            // graph.connect() ne représente une voie matérielle.
        }
    };

    class HeadphoneTrim final : public Node {
    public:
        explicit HeadphoneTrim(NodeID id) : Node(id, stereoLayout()) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (ChannelIndex c = 0; c < stereoChannels; ++c)
                for (FrameCount f = 0; f < ctx.frames; ++f) {
                    const float sample = 0.5F * ctx.inputs[c].buffer[f];
                    ctx.outputs[c].buffer[f] = sample;
                    ctx.backendOutputs[c].buffer[f] = sample;
                }
            // La dépendance Routeur -> Casque fixe l'ordre des deux writers.
            // Ne pas remettre toutes les voies à zéro ici : 2/3 doivent rester.
        }
    };

    void connectStereo(DAG& graph, NodeID from, NodeID to, PortType input = PortType::In) {
        for (ChannelIndex c = 0; c < stereoChannels; ++c)
            // ATTENTION : connect(destination, source), avec identités logiques.
            // graph.node(id)->channel(port,c) est une autre écriture de encode().
            checked(graph.connect(encode(to, input, c), encode(from, PortType::Out, c)), "Connexion");
    }

    /// Inspection SUR CONTRÔLE d'un plan Ready, avant publication, sans garder de vue.
    void describe(std::string_view name, const ExecutionPlan& plan) {
        const auto memory = plan.preparedMemoryUsage();
        std::cout << name << " : " << plan.tasks().size() << " Nodes, " << plan.bufferSlotCount()
                  << " slots, " << plan.delayTaskCount() << " DelayTasks, " << memory.historyBytes
                  << " octets d'historiques PDC, " << memory.totalBytes << " octets de plan\n";
        for (const auto& node : plan.pdcAnalysis()->nodes)
            std::cout << "  Node " << node.node << " : P=" << node.intrinsic
                      << " A=" << node.arrival << " L=" << node.cumulative << '\n';
        for (const auto& op : plan.operationSchedule())
            if (op.kind == OperationKind::Delay)
                std::cout << "  Compensation " << op.connection.from << " -> " << op.connection.to
                          << " : D=" << op.delaySamples << " samples\n";
        // P=latence DSP, A=max L des prédécesseurs, L=A+P, D=A-L(source).
        // Les slots sont recyclés après le dernier lecteur, pas les historiques.
        // Le budget du plan exclut le DSP des Nodes et les temporaires contrôle.
    }

    struct Bank {
        std::array<std::array<float, maxFrames>, 2> input{};
        std::array<std::array<float, maxFrames>, 6> output{};
    };

    /// Remplace un driver : buffers possédés par le backend, deux banques alternées.
    class FakeBackend {
    public:
        const Bank& callback(PlanSwitcher& player, FrameCount frames) noexcept {
            auto& bank = banks_[nextBank_++ % banks_.size()];
            bank.input[0].fill(1.F);
            bank.input[1].fill(-1.F);
            for (auto& lane : bank.output) lane.fill(-99.F);
            const std::array<const float*, 2> inputs{bank.input[0].data(), bank.input[1].data()};
            const std::array<float*, 6> outputs{bank.output[0].data(), bank.output[1].data(),
                bank.output[2].data(), bank.output[3].data(), bank.output[4].data(), bank.output[5].data()};

            // AUDIO : un appel borné, sans compilation, log ou gestion d'erreur texte.
            // Capacités explicites ; les adresses peuvent changer à chaque bloc.
            player.exec(mka::audio::AudioProcessContext{inputs, outputs, frames, maxFrames, maxFrames});
            return bank; // Inspection du résultat ensuite, hors callback.
        }
    private:
        std::array<Bank, 2> banks_{};
        std::size_t nextBank_ = 0;
    };

    /// Oracle hors audio : Gain 0.5 + deux branches alignées = entrée retardée.
    void render(FakeBackend& backend, PlanSwitcher& player, LatencySamples delay,
                std::size_t& elapsed, std::size_t samples) {
        constexpr std::array<FrameCount, 5> sizes{0, 1, 7, 64, maxFrames};
        const auto end = elapsed + samples;
        for (std::size_t block = 0; elapsed < end; ++block) {
            const auto frames = static_cast<FrameCount>(std::min<std::size_t>(sizes[block % sizes.size()], end - elapsed));
            const auto& bank = backend.callback(player, frames);
            for (std::size_t c = 0; c < bank.output.size(); ++c)
                for (FrameCount f = 0; f < maxFrames; ++f) {
                    float expected = -99.F; // Au-delà de frames : pas de write.
                    if (f < frames) {
                        const float signal = elapsed + f < delay ? 0.F : (c % 2 ? -1.F : 1.F);
                        expected = c < 2 ? 0.5F * signal : c < 4 ? signal : 0.F;
                    }
                    if (bank.output[c][f] != expected)
                        throw std::runtime_error("Rendu inattendu : PDC, routage ou capacité");
                }
            elapsed += frames; // Un bloc vide ne fait pas avancer les histoires.
        }
    }
}

int main() try {
    // 1. CONTRÔLE : DAG possède les Nodes ; les plans les empruntent.
    // Déclarer le DAG avant le switcher garantit le bon ordre de destruction.
    DAG graph;
    const auto source = checked(graph.add<StereoSource>(), "Source");
    const auto gain = checked(graph.add<StereoGain>(0.5F), "Gain");
    auto effect = checked(graph.add<LatentEffect>(64), "Effet 64 samples");
    const auto mix = checked(graph.add<StereoMix>(), "Mix");
    const auto router = checked(graph.add<OutputRouter>(), "Routeur");
    const auto headphones = checked(graph.add<HeadphoneTrim>(), "Casque");
    connectStereo(graph, source, gain);
    connectStereo(graph, gain, effect);
    connectStereo(graph, gain, mix); // Fan-out : un producteur, plusieurs lecteurs.
    connectStereo(graph, effect, mix, PortType::Aux);
    connectStereo(graph, mix, router);
    connectStereo(graph, router, headphones);
    checked(graph.markAsOutput(router), "Sortie routeur");
    checked(graph.markAsOutput(headphones), "Sortie casque");

    expectRefusal(graph.connect(encode(source, PortType::In, 0), encode(router, PortType::Out, 0)),
                  ErrorCode::CycleDetected, "Boucle interdite");
    std::cout << "Ordre topologique :";
    for (const auto id : checked(topologicalOrder(graph), "Tri topologique")) std::cout << ' ' << id;
    std::cout << '\n';

    // 2. Préparer toutes les capacités avant de démarrer le backend.
    // Les entrées/sorties physiques doivent garder ces counts, même frames=0.
    BackendConfig config{.maxFrames = maxFrames, .inputChannels = 2, .outputChannels = 6,
        .sampleRate = 48000, .memoryBudgetBytes = 1024 * 1024,
        .slotStrategy = SlotStrategy::Reuse, .maxDelaySamples = 512};
    const CompileConfig limits{.maxNodes = 16, .maxConnections = 32, .memoryBudgetBytes = 1024 * 1024};
    // connectionDelays reste vide : le compilateur insère automatiquement le PDC.
    {
        // Pour un graphe fixe, compile(config) fournit directement un plan dont
        // exec(bloc) constitue le callback. Ici on l'inspecte, puis on le libère
        // avant de passer au switcher. Ne jamais exécuter simultanément deux
        // plans empruntant les mêmes Nodes DSP.
        auto fixedPlan = checked(graph.compile(config), "Compilation d'un plan fixe");
        describe("Plan fixe à 64 samples", fixedPlan);
    }
    PlanSwitcher player;
    checked(player.initialize(graph, config, limits), "Initialisation de A");
    FakeBackend backend;
    std::size_t elapsed = 0;
    render(backend, player, 64, elapsed, 512);
    expectRefusal(graph.setConfig(gain, stereoLayout()), ErrorCode::NodeInUse, "Config active figée");

    // 3. Éditer le DAG DÉSIRÉ. A conserve son propre snapshot et reste audible.
    // Une latence différente exige un NOUVEAU DSP préparé, pas une mutation du
    // Node partagé avec A. Fréquence et comportement restent figés sous emprunt.
    const auto oldEffect = effect;
    effect = checked(graph.add<LatentEffect>(256), "Nouvel effet 256 samples");
    for (ChannelIndex c = 0; c < stereoChannels; ++c)
        if (!checked(graph.disconnect(encode(mix, PortType::Aux, c), encode(oldEffect, PortType::Out, c)), "Déconnexion"))
            throw std::runtime_error("Connexion à retirer absente");
    checked(graph.remove(oldEffect), "Retrait désiré de l'ancien effet");
    connectStereo(graph, gain, effect);
    connectStereo(graph, effect, mix, PortType::Aux);
    // remove() enlève aussi ses arêtes ; le Node retiré reste vivant dans A.
    if (graph.collectRetiredNodes() != 0) throw std::runtime_error("Retrait avant ACK");

    auto tooSmall = config;
    tooSmall.memoryBudgetBytes = 0;
    expectRefusal(player.prepareInactivePlan(graph, tooSmall, limits), ErrorCode::MemoryBudgetExceeded, "Budget de B nul");
    render(backend, player, 64, elapsed, 64); // A continue, malgré l'édition/refus de B.

    // Candidat de diagnostic : un slot par signal, puis abandon sans publication.
    auto unique = config;
    unique.slotStrategy = SlotStrategy::Unique;
    checked(player.prepareInactivePlan(graph, unique, limits), "B sans réutilisation");
    describe("Candidat debug abandonné", *player.readyPlan());
    checked(player.discardReadyPlan(), "Abandon du candidat");

    // 4. Compiler/préparer B, inspecter ses valeurs immuables, puis publier.
    checked(player.prepareInactivePlan(graph, config, limits), "Préparation de B");
    describe("B avec réutilisation des slots", *player.readyPlan());
    checked(player.publishReadyPlan(), "Publication de B");
    expectRefusal(player.prepareAndPublish(graph, config, limits), ErrorCode::PlanBusy, "B déjà Pending");
    // prepareAndPublish() est le raccourci des deux appels précédents lorsqu'on
    // ne souhaite pas inspecter le plan Ready. Pending ne doit jamais être écrasé.

    // Le prochain exec adopte B AVANT ses traitements, même pour zéro frame.
    (void)backend.callback(player, 0);
    checked(player.retireAcknowledgedPlan(), "ACK et libération de A sur contrôle");
    if (graph.collectRetiredNodes() != 1) throw std::runtime_error("Ancien effet non libéré après ACK");
    elapsed = 0;
    render(backend, player, 256, elapsed, 512);
    // Les historiques PDC de B et de son nouvel effet commencent à zéro :
    // warmup/silence initial autorisé, pas de migration ni crossfade en V1.

    // 5. Pause/reprise : dans ce backend synchrone aucun callback ne subsiste.
    // Avec un driver réel : stop + join d'abord, resume avant de le redémarrer.
    checked(player.stopAfterBackendStopped(), "Pause");
    checked(player.resumeAfterBackendStopped(), "Reprise");
    render(backend, player, 256, elapsed, 64); // L'histoire chaude est conservée.
    if (player.takeCallbackDiagnostics() != 0) throw std::runtime_error("Diagnostic audio inattendu");

    // Refus audio borné : frames > maxFrames, mais buffers de 128 samples connus.
    // Pas de log/exception depuis exec : silence et bits, drainés sur contrôle.
    const auto& rejected = backend.callback(player, maxFrames + 1);
    for (const auto& lane : rejected.output)
        if (!std::all_of(lane.begin(), lane.end(), [](float v) { return v == 0.F; }))
            throw std::runtime_error("Bloc rejeté non silencieux");
    const auto flags = player.takeCallbackDiagnostics();
    if (!(flags & static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded)))
        throw std::runtime_error("Diagnostic de capacité manquant");
    std::cout << "Diagnostic attendu : " << formatCallbackDiagnostics(flags) << '\n';

    // Les rôles output sont aussi des éditions désirées : B reste inchangé.
    checked(graph.unmarkAsOutput(router), "Retrait du rôle output routeur");
    checked(graph.unmarkAsOutput(headphones), "Retrait du rôle output casque");
    render(backend, player, 256, elapsed, 64);

    // 6. Plus aucun callback présent/futur : libérer les plans AVANT leurs Nodes.
    checked(player.shutdownAfterBackendStopped(), "Arrêt définitif");
    for (const auto id : {headphones, router, mix, effect, gain, source})
        checked(graph.remove(id), "Destruction sur contrôle");
    std::cout << "Recette réussie : PDC, casque/enceintes, LWW, silence, swap et arrêt.\n";
    return 0;
} catch (const std::exception& error) {
    // Contrôle uniquement. Le backend simulé ne possède aucun thread à joindre.
    std::cerr << "Échec de l'exemple : " << error.what() << '\n';
    return 1;
}
