# Architecture générale du moteur DSP

## A. Vue d'ensemble des couches

Couche 0 (`mka::audio`) : backends ALSA/JACK/PipeWire, ultérieurement WASAPI/CoreAudio, choix d'endpoint, normalisation vers des buffers audio flottants par canal, callback `AudioProcessContext`. Couche 1 (`mka::audio::dsp`) : édition/validation du DAG, compilation, plans RT, mémoire et PDC. La couche 1 **ne dépend pas directement** des bibliothèques backend, uniquement de la forme normalisée du contexte audio. Le futur DAW pilote la couche DSP depuis le contrôle, sans appeler `process()` à la place du backend.

```mermaid
flowchart TD
 App["Application / DAW / éditeur UI"] --> G["DAG : structure souhaitée"]
 G --> C["DAG::compile : non RT"]
 C --> DB["DoubleBuffer<ExecutionPlan>"]
 B["Backend::callback(ctx)"] --> RT["RT: trySwap + exec(ctx)"]
 DB --> RT
 RT --> N["Node::process(ProcessContext)"]
 RT --> DP["DelayTask PDC"]
 RT --> BP["BufferPool du plan actif"]
```

## B. Modèle de classes complet (proposition de structuration)

```mermaid
classDiagram
 direction TB
 class PortType {
   <<enumeration>>
   In
   Out
   Aux
 }
 class ChannelID {
   <<value type>>
   +value : uint64_t
   +node() NodeID
   +port() PortType
   +index() uint16_t
 }
 class Connection {
   <<struct>>
   +from : ChannelID
   +to : ChannelID
 }
 class Config {
   <<struct>>
   +layoutInput : vector~uint16_t~
   +layoutAuxiliary : vector~uint16_t~
   +layoutOutput : vector~uint16_t~
 }
 class Node {
   <<abstract>>
   -id_ : NodeID
   -config_ : Config
   +process(ctx) noexcept
   +latencySamples() uint32_t
   +channel(port, index) ChannelID
 }
 class DAG {
   -nodes_ : map~NodeID,unique_ptr~Node~~
   -connections_ : vector~Connection~
   -outputsNodes_ : vector~NodeID~
   +add() NodeID
   +remove(id) Result
   +connect(to, from) Result
   +disconnect(to,from) Result
   +markAsOutput(id) Result
   +compile(plan, cfg) Result
 }
 class Channel {
   <<struct>>
   +channel : ChannelID
   +buffer : SamplePointer
 }
 class ProcessContext {
   <<struct>>
   +inputs : ReadOnlyChannelList
   +auxiliary : ReadOnlyChannelList
   +outputs : MutableChannelList
   +frames : uint32_t
 }
 class ExecutionTask {
   <<struct>>
   +node : Node*
   +context : ProcessContext
 }
 class DelayTask {
   <<compiled internal>>
   +source : const float*
   +destination : float*
   +delaySamples : uint64_t
   +history : DelayLine
 }
 class BufferPool {
   -storage : AlignedFloatStorage
   -capacitySlots : uint32_t
   -maxFrames : uint32_t
   +prepare() Result
   +buffer(id) float*
 }
 class ExecutionPlan {
   -tasks_ : vector~ExecutionTask~
   -pool_ : BufferPool
   -delays_ : vector~DelayTask~
   -sourceTaskIndices_ : vector~TaskIndex~
   -outputTaskIndices_ : vector~TaskIndex~
   +exec(ctx) noexcept
 }
 class ExchangeState {
   <<enumeration>>
   Idle
   Pending
   Acknowledged
 }
 class DoubleBuffer {
   -plans_ : ExecutionPlan[2]
   -state_ : atomic~ExchangeState~
   -activeIndex_ : atomic~uint8_t~
   +publish() Result
   +front() ExecutionPlan&
   +back() ExecutionPlan&
 }
 DAG "1" *-- "0..*" Node : owns
 DAG "1" *-- "0..*" Connection
 Node *-- Config
 Connection --> ChannelID
 ChannelID --> PortType
 ExecutionPlan "1" *-- "0..*" ExecutionTask
 ExecutionPlan "1" *-- "1" BufferPool
 ExecutionPlan "1" *-- "0..*" DelayTask
 ExecutionTask *-- ProcessContext
 ExecutionTask --> Node : borrows
 ProcessContext o-- Channel : views
 DoubleBuffer "1" *-- "2" ExecutionPlan
 DoubleBuffer --> ExchangeState
```

**Lecture des symboles UML :** `*--` composition (objet propriétaire), `o--` agrégation/vue dépendante, `-->` référence. `Node` est possédé par le DAG : le plan ne doit jamais le détruire. `ProcessContext` est la structure de descripteurs possédée par une tâche, mais ses pointeurs audio sont non propriétaires. `SamplePointer` est un raccourci UML pour `const float*` en entrée et `float*` en sortie, pas un type C++ unique validé.

## C. Classification des processus et accès

| Opération | Thread contrôle | Thread audio | Effets de bord / durée |
|---|---|---|---|
| `DAG::add/remove/connect/disconnect/markAsOutput` | Oui | Non | Modifie le graphe souhaité ; suppression physique soumise à l'absence de références dans tous les plans |
| `Node::channel` / validation IDs | Oui | Pas nécessaire | Pure construction logique ; pas d'accès aux samples |
| `Node::latencySamples` | Oui, à compilation | Non, dans la V1 | Propriété déclarative ; valeur figée dans le plan |
| `DAG::compile` | Oui | Non | Alloue, calcule topologie, PDC, pool, contextes ; peut échouer |
| `BufferPool::prepare` | Oui | Non | Alloue, initialise, peut échouer |
| `DoubleBuffer::publish` | Oui | Non | Publication atomique, pas de nettoyage du plan actif |
| `DoubleBuffer::trySwap` | Non | Oui, frontière de bloc | Lit état atomique, adopte plan prêt ; aucune attente |
| `ExecutionPlan::bindInputs/Outputs` | Non | Oui | Actualise seulement descripteurs préalloués vers buffers backend du callback |
| `ExecutionPlan::exec` | Non | Oui | Efface sorties matérielles, raccorde, exécute la séquence sans allocation |
| `Node::process` | Non | Oui | Lit entrées/aux, écrit sorties ; peut conserver son état algorithme |
| `DelayTask::process` | Non | Oui | Lit/écrit historique circulaire propre au plan |
| `DoubleBuffer::releaseRetired` | Oui | Non | Détruit ancien plan après ACK et libère ressources |

## D. Propriété des données (ownership)

```mermaid
flowchart TB
 G["DAG (propriétaire)"] -->|unique_ptr| N["Node et Config"]
 G -->|valeurs| E["Connection / output marks"]
 DB["DoubleBuffer (propriétaire)"] --> A["Plan A"]
 DB --> B["Plan B"]
 A -->|propriétaire| P["Pool + Task descriptors + Delay histories"]
 B -->|propriétaire| Q["Pool + Task descriptors + Delay histories"]
 P -.->|non owning Node*| N
 Q -.->|non owning Node*| N
 X["AudioProcessContext backend (propriétaire externe)"] -.->|pointeurs callback seulement| P
 X -.->|pointeurs callback seulement| Q
```

**Règle fondamentale :** les pointeurs backend sont valides seulement pendant le callback ; les pointeurs vers le pool sont valides tant que le plan les possède et n'est pas réinitialisé ; les `Node*` restent valides tant qu'aucun plan ne les utilise. Les vues `std::span` vers des listes de descripteurs restent valides tant que ces listes ne sont pas redimensionnées/déplacées.

## E. Cycle de vie complet

```mermaid
sequenceDiagram
 autonumber
 participant UI as App/UI
 participant G as DAG
 participant C as Compilation contrôle
 participant DB as DoubleBuffer
 participant BA as Backend callback RT
 participant P as Plan actif
 UI->>G: ajouter/configurer/connecter des Node
 UI->>G: désigner les sorties
 UI->>C: demander compile(maxFrames, ioConfig)
 C->>G: lire topologie validée
 C->>C: tri topologique, liveness, PDC
 C->>DB: construire back plan B
 C->>DB: publish(Pending/release)
 BA->>DB: load Pending/acquire, swap, ACK
 BA->>P: exec(backendCtx)
 P->>P: zero outputs, bind sources/sinks
 P->>P: sequence de tasks + delays
 P-->>BA: audio terminé
 UI->>DB: constater Acknowledged
 DB->>DB: retirer ancien plan hors RT
```

## F. V1 vs extensions

**Dans V1 :** séquence de tâches mono-thread RT, sources avec toutes les entrées backend en `const`, sorties désignées avec toutes les sorties en `float*`, buffers internes mono float32, in-place conservateur, PDC sample-précis, recompilation/publish atomique. **Hors V1 :** parallélisme d'effets, compensation intégrée à MIDI/automation, transitoires strictement sans clic lors des swaps, drainage audible des tails, PDC entre interfaces physiques à horloges indépendantes, gestion arbitraire des layouts multicanaux standardisés.
