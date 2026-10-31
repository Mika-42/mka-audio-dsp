# Matrice de traçabilité des exigences V1

Cette matrice relie les décisions, les modules, les algorithmes et les validations. Les IDs `JD-xxx` renvoient au [journal](../00-reference/JOURNAL_DE_DECISIONS.md).

| Exigence | Décision | Modules responsables | Tests |
|---|---|---|---|
| Arêtes structurales, pas de cycles ni double occupation | JD-006 | Identifiants, DAG | G-01..G-10 |
| Trois ports In/Out/Aux et identifiants compacts | JD-004/005 | Identifiants | G-01..G-04 |
| Un seul thread audio, plan topologique | JD-001/002/003 | DAG, ExecutionPlan | SW-01, RT instrumentation |
| `frames` effectif ≤ `maxFrames` | JD-010 | ExecutionPlan, Pool | B-01..B-03 |
| Slots mono float32 préparés hors RT | JD-011/012 | BufferPool | B-01/B-10 |
| Silence stable et non mutable | JD-012 | BufferPool, ProcessContext | B-04 |
| `lastUse` statique, fan-out sûr | JD-013 | Compilation, BufferPool | B-05/B-06/B-09 |
| In-place seulement si prouvé | JD-014 | Node, BufferPool | B-07 |
| Processeur état interne distinct du pool | JD-015 | Node, DelayTask | B-08/P-04 |
| Double plan + handshake | JD-016/017 | DoubleBuffer | SW-01..SW-08 |
| Node non détruit si référencé | JD-018 | DAG, DoubleBuffer | SW-06 |
| Échec de compilation conserve plan actif | JD-019 | Compilation, DoubleBuffer | SW-05, P-10 |
| Sources voient tous les inputs const | JD-025/026 | ProcessContext, ExecutionPlan | IO-01..IO-05 |
| Sorties désignées voient tous outputs | JD-027 | DAG, ExecutionPlan | IO-06..IO-08 |
| Last writer wins, sans mix automatique | JD-028/030 | Node, ExecutionPlan | IO-07 |
| Latence déclarée par Node | JD-031/032 | Node, plugins | P-01..P-03/P-12 |
| PDC analysé à compilation | JD-033/034 | DAG::compile, PDC | P-02/P-03/P-07 |
| DelayLine persistante, sample-exacte | JD-035/036 | DelayTask | P-04..P-07 |
| Latence des chemins vers outputs | JD-037 | Compilation/PDC | P-09, limitations explicites |
| Changement de plan avec PDC, sans promesse de transition inaudible | JD-038 | DoubleBuffer/DelayTask | P-11 |

## Exigences non couvertes par choix de périmètre

Tails audibles après suppression (JD-020) ; exécution parallèle (JD-002) ; mapping hardware par `ChannelID` (JD-024) ; exclusivité de canaux output (JD-030) ; migration d'historiques / crossfade entre plans (JD-038) ; compensation MIDI/automation/latence hardware (JD-039). L'absence de test de ces comportements n'est pas un défaut V1 puisque ces fonctions sont explicitement exclues.
