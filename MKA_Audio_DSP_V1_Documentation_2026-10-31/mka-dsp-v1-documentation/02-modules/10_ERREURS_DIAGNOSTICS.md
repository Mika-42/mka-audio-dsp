# Module 10 — Erreurs, diagnostics, limites et API non-RT

## Politique générale

Les opérations de configuration/édition/compilation sont effectuées hors RT et renvoient `std::expected<T, Error>` ou l'alias `Result = std::expected<void, Error>`. La nature exacte de `Error` a été discutée antérieurement : enum d'erreurs ou objet avec message détaillé. **Proposition** : structurer `Error` avec un code stable, un message informatif facultatif hors RT et éventuellement un contexte (`NodeID`, `ChannelID`). Ne pas construire de messages détaillés dans le callback.

```cpp
enum class ErrorCode : std::uint16_t {
    InvalidParameter,
    NodeNotFound,
    InvalidDirection,
    ConnectionExists,
    InputOccupied,
    CycleDetected,
    NodeInUse,
    CapacityExceeded,
    ArithmeticOverflow,
    PrepareFailed,
    LatencyOverflow,
    PlanBusy
};

struct Error {
    ErrorCode code;
    std::string message;  // Non-RT only.
};

using Result = std::expected<void, Error>;
```

Les noms supplémentaires `PlanBusy`, `LatencyOverflow` sont **propositions** ; les catégories historiques incluent `CycleDetected`, `NodeInUse`, `CapacityExceeded`, invalid transitions et erreurs de préparation. Une `std::expected` n'implique pas qu'aucune fonction auxiliaire C++ ne pourrait lancer en cas de mauvaise allocation ; le compilateur doit convertir les échecs en erreurs convenables hors RT, sans laisser le plan partiellement publié.

## Tableau de responsabilités

| Situation | Détection | Réaction |
|---|---|---|
| Arête invalide | `DAG::connect` sur contrôle | Refus, état du DAG inchangé |
| Cycle | `DAG::connect` / `compile` | `CycleDetected`, pas de publication |
| Entrée occupée | `DAG::connect` | `InputOccupied`, pas de mix implicite |
| Nœud référencé à la suppression | DAG + deux plans | `NodeInUse` ou retrait différé sous contrôle |
| PDC impossible (overflow/ressources) | `compile()` | Erreur, plan A conservé |
| Pool insuffisant | `BufferPool::prepare` | Erreur, plan A conservé |
| Plan en vol (`Pending`) | `publish` contrôle | Refus/batching des éditions souhaitées |
| `frames > maxFrames` | `exec` RT | Effacement des sorties, drapeau atomique, retour sans exécuter les Node |
| Canaux backend dépassent capacité préparée | `exec` RT | Silence + drapeau de reconfiguration |
| Double écriture sur sortie matérielle | Pas de vérification par design | LWW dans ordre RT déterministe ; responsabilité application |
| Erreur d'un plugin temps réel | Contrat plugin | Doit rester `noexcept` et RT-safe, réponse de sécurité à formaliser |

## Diagnostics RT proposés

Utiliser un masque `std::atomic<std::uint32_t>` de bits coalescés (`TooManyFrames`, `TooManyInputChannels`, `TooManyOutputChannels`, `BackendInvalid`) stocké en `memory_order_relaxed` si aucune donnée non atomique n'est publiée avec. Le contrôle le lit régulièrement et déclenche une reconfiguration. Il ne doit pas effectuer de log synchrone depuis RT. Ce mécanisme est une **proposition** et ne constitue pas une obligation historique sur le format exact du diagnostic.

## Contraintes API

Toutes les boucles/conditions C++ possèdent des accolades (style projet). Documenter Doxygen sur les contrats publics, notamment `@thread`/restrictions, durée de vie des pointeurs, gestion d'erreurs, latence et nombre de samples. Chaque test d'erreur doit vérifier **l'absence de mutation** du DAG/plan lorsqu'une opération est refusée, pas seulement le code retour.
