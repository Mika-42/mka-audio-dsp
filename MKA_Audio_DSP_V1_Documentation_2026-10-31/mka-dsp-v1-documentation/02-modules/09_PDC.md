# Module 09 — PDC : latence déclarée, `DelayTask` et `DelayLine`

## Enjeu

Dans un DAG, un chemin avec plugin lookahead peut produire un signal retardé par rapport à un chemin rapide. À un point de jonction, PDC ajoute une durée de mémoire au chemin rapide pour aligner leurs temps audio. Il n'« avance » aucun signal et n'est ni un `sleep` ni une attente du thread RT. La compensation est calculée au contrôle et exécutée sample par sample ou par blocs dans le plan.

## Contrat source de latence

`Node::latencySamples() -> std::uint32_t` (**proposition exacte de signature**, validée en intention) retourne la latence algorithmique entière déclarée. Valeur 0 par défaut, lue lors de `compile`, pas calculée automatiquement à partir du CPU ou du nombre de buffers internes. Un plugin VST3 dispose d'un mécanisme `getLatencySamples`; les autres adaptateurs doivent traduire les conventions propres à leur SDK. Une latence changeante doit être figée par génération et coordonnée avec l'état réel du plugin.

## Types compilés suggérés

```cpp
struct DelayLine {
    std::vector<float> history;  // Prepared non-RT, persistent across callbacks.
    std::size_t cursor = 0;      // RT-owned while active.
};

struct DelayTask {
    const float* source = nullptr; // Plan buffer, non-owning.
    float* destination = nullptr;  // Plan buffer, non-owning.
    std::uint64_t delaySamples = 0;
    DelayLine line;                // Owned by plan; prepared non-RT.
};
```

Types des membres : `source` lecture seule, `destination` écriture, `history` écriture RT et propriété du plan, `cursor` index RT, `delaySamples` constant après compilation. Une implantation peut stocker les DelayLine en mémoire contiguë du plan plutôt qu'un vecteur par tâche. Une DelayTask par canal mono de connexion est un point de départ simple ; des lignes stéréo peuvent être regroupées par optimisation, sans changer la sémantique.

## Formule

Pour entrée `x[n]`, sortie `y[n]`, retard entier D ≥ 0 : `y[n] = x[n-D]` ; au démarrage les échantillons négatifs `x[n<0]` valent 0. D=0 → connexion directe, pas d'état. Pour D>0, l'historique possède exactement D samples, préremplis de 0. Le callback peut contenir n'importe quel nombre de frames `<=maxFrames` ; le `cursor` persiste.

```mermaid
flowchart TD
 A[DelayTask D samples] --> B{D = 0 ?}
 B -->|oui| C[Pas de task, pointeur direct]
 B -->|non| E[prepare history[D] = 0 hors RT]
 E --> F[Chaque callback, pour chaque sample]
 F --> G[output = history[cursor]]
 G --> H[history[cursor] = input]
 H --> I[cursor = cursor+1 modulo D]
```

### Exemple cross-callback

`D=3` ; callback 1 `in=[1,2,3,4] -> out=[0,0,0,1]` ; callback 2 `in=[5,6,7,8] -> out=[2,3,4,5]`. La delay line ne reçoit pas un nouvel ensemble de 3 zéros au second callback.

## Durée de vie des buffers et in-place

`DelayTask::source` peut être un slot partagé en lecture ; `destination` est un slot distinct possédé par le plan et vivant jusqu'à la dernière lecture de son consommateur. La ligne historique doit survivre à tous les callbacks du plan actif. Par sûreté, V1 n'autorise pas l'aliasing `source==destination` pour une ligne de retard tant qu'une implémentation précisément testée n'a pas documenté ce cas. Une delay line sur une branche fan-out ne doit pas réécrire la sortie partagée de l'ancien producteur.

## Délai cumulé et insertion

Voir [algorithme PDC](../03-algorithmes/PDC.md). À chaque jonction, calcul de la latence maximale et ajout de `DelayTask` sur les branches plus rapides. Le compilateur inclut ces opérations dans la séquence et le placement mémoire. Les sorties marquées peuvent être alignées par chemin, mais pas prouvées par canal physique si leurs `process()` font des écritures libres et non déclarées.

## Contours et difficultés

- Sommes en `uint64_t` avec contrôle de dépassement, limite mémoire vérifiée avant allocation.
- Latences en samples au sample rate **effectif** du plan ; une modification de fréquence implique une nouvelle préparation.
- Un plugin n'a qu'une latence scalaire commune à ses canaux dans ce contrat simplifié ; traitements aux décalages dépendant de l'entrée ne sont pas entièrement descriptibles.
- Changement de latence à chaud : recompile et publish ; B initialise son historique à zéro. Migration d'état/crossfade non garantis V1.
- Latence de transport/convertisseurs physiques, dérive de plusieurs endpoints, événements MIDI et paramètres non alignés : hors PDC audio V1.
- `TailInfo` (queue sonore) n'est pas le nombre d'échantillons de latence ; pas de drainage automatique après suppression.

## Tests

Impulsion sur deux branches, latences 0/64/256/512, D=1 et D>maxFrames, blocs variant 1/128/64, fan-out avec compensation différente par branche, cross-callback, historique initial nul, buffer memory limit, change de plan conservateur, erreur déclaration incorrecte simulée, latence à l'entrée Aux, latence globale aux outputs terminaux connus, aucune allocation RT.
