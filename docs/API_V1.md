# API V1 et contrats de propriété

Ce contrat décrit le code actif C++26 ; les archives de conception et leurs maquettes ne remplacent pas les signatures des modules. Doxygen complète les règles ci-dessous avec chaque définition publique. Les types `detail::*` exportés pour les modules/tests sont des détails de compilation, sans stabilité ABI garantie.

## Types, formats et polymorphisme

Toutes les valeurs DSP sont dans `mka::audio::dsp`, sauf `mka::audio::AudioProcessContext`. Les entiers sont non signés, en samples entiers pour les temps audio et en Hz pour sampleRate. Aucun temps CPU n'est une latence PDC. Pointeurs et `size_t` dépendent de la plateforme ; les agrégats ne sont pas un format de sérialisation.

| Types/symboles publics | Format et propriété | Thread et durée |
|---|---|---|
| NodeID, ChannelID, ChannelIndex | uint32/uint64/uint16, valeurs sans owner ; NodeID zéro invalide, encodage node32/port16/index16 | Copies sans allocation, IDs non réutilisés tant que référencés |
| FrameCount, LatencySamples, BufferId, GenerationID, SignalID | uint32, uint32, uint32, uint64, size_t | FrameCount effectif distinct de maxFrames ; A/L/D en uint64 |
| PortType, ErrorCode | enum class uint16 ; In=0/Out=1/Aux=2 ; erreurs 0..13 stables | Valeurs seules, pas de log |
| CallbackDiagnostic | enum class uint32, masques ; atomics uint32 lock-free vérifiés | Accumulation relaxed, drain sur contrôle |
| HandshakeState, PlanSlotState | enum class uint8 | Champs de contrôle distincts du handshake atomique |
| OperationKind, SlotStrategy | enum class de sous-jacent int, sans sérialisation ABI | Node/Delay/Readback et Reuse/Unique, compilation contrôle |
| Error, GraphError, CompileError, PrepareError, ErrorType, Result | Un seul Error{code,node,channel}, trivial ; alias expected<void,Error> | Refus numérique hors RT ; champs inconnus à zéro |
| Config et Node::Config | Même type, vectors de ChannelIndex ordonnés et indépendants | Snapshot propre au Node, immuable pendant emprunts ; validateConfig sans heap |
| Connection, ConnectionDelay | Identités ChannelID complètes ; D uint64 | Valeurs sans samples ni hardware mapping |
| InputChannel / OutputChannel | Descriptor avec const float* / float*, propriété des samples externe | Lecture seule / écriture autorisée, voir catalogue ci-dessous |
| Channel (legacy), Port::FL/FR/FC/LFE/SL/SR/BL/BR | Channel est un agrégat hérité non validé nodeID uint64, channel uint64 et PortType uint16, sans pointeur ni samples ; constantes Port uint32, indices logiques et non matériels | Valeurs copiées ; utiliser ChannelID/Connection pour les identités actives et InputChannel/OutputChannel pour les buffers |
| ProcessContext / PreparedContextBindings | Spans empruntés de descriptors, frames uint32 ; pas de propriétaire audio | Une invocation pour le premier, stockage plan stable pour le second |
| AudioProcessContext / BackendBlock | Vues physiques empruntées ; capacités explicites pour AudioProcessContext | Un callback, jamais conservées après exec |
| Node | Classe abstraite non finale, process pur virtuel noexcept, latence virtuelle const noexcept zéro, dtor virtuel | DAG propriétaire unique, adresses stables ; pas de copie/move ; process audio, construction/destruction contrôle |
| DAG | Owner non copiable des Nodes désirés et retirés | Un contrôle sérialisé ; doit survivre à tous les plans ; retrait désiré immédiat et destruction différée |
| ExecutionTask, TaskInfo, PlanOperationInfo | Vue Node/context et métadonnées numériques de compilation | Emprunts du plan ; vues expirent au remplacement/retrait, jamais owner de Node |
| ExecutionPlan | Classe finale concrète ; possède pool, descriptors, opérations, snapshots et lignes ; emprunte Nodes | exec noexcept audio unique/non réentrant ; copie/move/prepare/retrait seulement sur contrôle après quiescence |
| PlanSwitcher | Classe finale concrète, ni copie ni move ; deux plans stables | Contrôle sérialisé + un audio ; backend stoppé/joint avant arrêt, destruction ou reprise |
| BufferPool | Classe finale, slots et silence mono float32 indépendants | prepare/copie contrôle, samples actifs audio ; aucun redimensionnement RT |
| DelayLine / DelayLineLimits | Classe finale concrète, vector de D floats et curseur ; quotas uint64/size_t | prepare/copie/move/dtor contrôle ; process noexcept un seul propriétaire audio |
| detail::DelayTask | Struct concret non polymorphe, const source / destination mutable + D uint64 + ligne propre | Préparation/binding contrôlés, execute noexcept audio ; aucun Node/backend conservé |
| CompileConfig / BackendConfig | Quotas, Hz, maxFrames ; counts uint32 ; memoryBudget size_t et maxDelay uint64 | Copiés à la compilation ; connectionDelays est emprunté uniquement pendant prepare |
| IntrinsicLatency, LatencySnapshot, NodeLatency, PdcAnalysis | P uint32, A/L/D uint64 ; tables possédées | Valeurs figées par génération, jamais relues depuis le DSP actif |
| SignalOperation, SignalLifetime, SlotAssignment | Productions denses, intervalles inclusifs, BufferId uint32 | Calculs contrôle ; ni historique ni silence dans la freelist |
| MemoryArray, PlanMemoryRequest, PlanMemoryUsage | Counts uint64, tailles/padding size_t, spans empruntés au calcul | Contrôle, sans allocation pour mesure ; budget retenu exclut temporaires/allocator/Nodes |

Aucune classe concrète de pool, ligne, plan ou switcher ne nécessite de RTTI/dynamic_cast. Le seul dispatch DSP est Node::process. Copier deux plans ne clone pas l'état DSP des Nodes : ils ne doivent pas traiter les mêmes instances concurremment.

## Catalogue des pointeurs audio

- Les tableaux physiques appartiennent au backend. Leurs samples input sont const ; les outputs sont mutables. Les descriptors physiques ont ChannelID=0 ; c'est la position dans le tableau que le Node choisit. Tout Node output reçoit toutes les sorties. Les vues sont effacées après exec.
- Une source topologique reçoit tous les inputs physiques, même avec layoutInput vide. Un Node non source reçoit ses In internes ; Aux reste interne. Une dépendance hors Config contraint la topologie mais ne produit pas de lecteur audio inexistant.
- Les Out logiques restent dans BufferPool, y compris pour un Node marqué output ayant un successeur. Les pointeurs/spans fixes sont valides tant que le plan est possédé et non remplacé. Leur contenu peut être réutilisé après lastUse : ne pas lire un ancien slot après son dernier lecteur.
- DelayTask.source lit un slot interne, destination écrit un autre slot ; validateDelayBinding refuse nulls non vides et recouvrements. L'historique privé appartient à la ligne de cette génération, sans pointer backend ni recyclage par slots.
- Source et destination externes couvrent frames, sans alias avec stockage écrit. frames=0 ne touche aucun sample et accepte nulls selon le contrat de shape exact. AudioProcessContext donne les capacités ; en rejet seuls les outputs effectivement annoncés, non nuls et bornés peuvent être mis au silence.

## Méthodes et refus

| Famille de méthodes | Paramètres/retour et erreurs | Garantie |
|---|---|---|
| encode/decode*, isValidPort, toChannelIndex | Identités/indices, conversion validée avant narrowing ; expected pour valeurs hors domaine | Aucun sample/owner touché, pas de heap |
| DAG::add/node/connect/disconnect/remove/setConfig/markAsOutput/unmarkAsOutput | connect(destination,source), ChannelIDs complets ; expected pour identité/port/direction/occupation/cycle/quota/in-use | Refus conserve la structure concernée ; remove réussi retire du désiré, conserve les Nodes encore empruntés |
| DAG::compile et topologicalOrder | CompileConfig/BackendConfig et cible inactive ; tri global min-NodeID | Refus garde cible/A, aucune publication ; compile structurel ne prépare/process aucun DSP |
| Node::id/config/channel/latencySamples/process | Métadonnées const sans allocation, latence scalaire samples, process void noexcept | Config et profil DSP stables sous emprunts ; constructeur peut allouer, aucune préparation DSP depuis callback |
| BufferPool::prepare et accessors | slots uint32, frames uint32, budget inclusif bytes ; expected | Taille/produits contrôlés avant allocation ; refus conserve stockage ; aucun redimensionnement dans buffer/silence |
| DelayLine::prepare(uint64_t) et surcharge Limits | D samples, quota samples/bytes ; expected PrepareError | Temporaire puis commit, zeros/cursor reset seulement au succès ; ArithmeticOverflow/MemoryBudgetExceeded/PrepareFailed |
| DelayLine::process / DelayTask::execute | Buffers distincts pour D>0, frames effectives ; void noexcept | y[n]=x[n−D], zéro avant n=0 ; aucun malloc/free/lock/log ; D=0 direct sans Task |
| ExecutionPlan::prepareBackend | Shape, connexions, outputs, cible inactive/quiescente ; expected | Capture P, A/L/D, finalise Node+Delay, vérifie lastUse, mémoire et binding avant commit ; refus ne touche A |
| ExecutionPlan::exec | AudioProcessContext canonique, adapters BackendBlock/ProcessContext ; noexcept | Shapes exactes même frames=0 ; silence borné et diagnostics si refus ; outputs zero une fois puis LWW par sample |
| PlanSwitcher initialize/prepareInactivePlan/publishReadyPlan/prepareAndPublish | Graph et config contrôlés ; expected | Pas de remplacement de Pending ; PlanBusy avant compile ; snapshots/DSP partagés immuables |
| retireAcknowledgedPlan / stopAfterBackendStopped / resumeAfterBackendStopped / shutdownAfterBackendStopped | expected ; précondition externe stop/join pour arrêt | Release/acquire pour transfert/ACK ; aucune libération audio ; arrêt sans callback annule Pending sous quiescence |
| Diagnostics et vues | callbackDiagnostics/takeCallbackDiagnostics noexcept atomiques ; vues const empruntées ; formatError/formatCallbackDiagnostics retournent string | Vues valides selon ownership ; formats peuvent allouer/lever seulement sur contrôle, aucun log implicite |
| Checked math, analyzeLatencies, calculateLifetimes, assign/validate slots | Tableaux de valeurs, cumuls uint64, taille plateforme ; expected | LatencyOverflow avant somme ; ArithmeticOverflow avant narrowing/produit ; mémoire/padding agrégés ; aucune publication partielle |

Autres catégories stables : InvalidParameter, NodeNotFound, InvalidPort, InvalidDirection, ConnectionExists, InputOccupied, CycleDetected, NodeInUse, CapacityExceeded, PlanBusy. Elles portent les identités connues sans construire de chaîne RT. Les callbacks ne lèvent pas ; les constructeurs/copies C++ et formateurs contrôle peuvent lever les exceptions de ressources documentées.

## Générations, PDC et exclusions

Un changement de latence/fréquence exige une autre instance préparée pour B ou la quiescence de tous les emprunts. Le snapshot figé d'un Node partagé ne doit pas être modifié en concurrence. Tous les In/Aux audio traitables participent à A=max L(pred), L=A+P et D=A−L(source), sans dépendre des canaux matériels librement écrits. Une déclaration de plugin fausse n'est pas automatiquement mesurée/réparée.

B commence avec ses lignes nulles ; les premières D frames de chaque nouveau trajet peuvent être silencieuses. Pas de migration/crossfade ni garantie de clic absent. Le callback ne attend pas le contrôle. Le backend réel doit empêcher tout callback futur avant destruction ; les flags atomiques ne remplacent pas un join.

Hors V1 : tails audibles après retrait, exécution DSP parallèle, MIDI/automation, latence matérielle, synchronisation inconnue par voie physique et plugins externes non instrumentés. Les maquettes `DoubleBuffer` correspondent aujourd'hui à PlanSwitcher. Un ancien `ctx.outputs` matériel doit être lu comme `ctx.backendOutputs` ; outputs est désormais uniquement logique. La matrice active de 04-validation et les tests nommés constituent les preuves exécutables, pas les exemples historiques.
