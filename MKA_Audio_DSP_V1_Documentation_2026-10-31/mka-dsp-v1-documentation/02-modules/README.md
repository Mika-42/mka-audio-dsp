# Fiches des modules

Chaque fiche définit : objectif, types et visibilité (`+` public, `-` privé), invariants, propriété/aliasing, thread autorisé, appels et erreurs, exemple, tests. Les types proposés pour les données dérivées sont des recommandations de modélisation et non une affirmation sur les noms déjà implémentés.

| Module | Fiche |
|---|---|
| `PortType`, `NodeID`, `ChannelID`, `Connection` | [01](01_IDENTIFIANTS_CONNEXIONS.md) |
| `Config`, `Channel`, `ProcessContext` | [02](02_CONFIG_CONTEXTE.md) |
| `Node`, contrat de plugin, `TailInfo`, latence | [03](03_NODE.md) |
| `DAG` et opérations d'édition | [04](04_DAG.md) |
| `BufferPool` et silence | [05](05_BUFFER_POOL.md) |
| `ExecutionTask`, `ExecutionPlan` | [06](06_EXECUTION.md) |
| Routage entrée/sortie backend | [07](07_ROUTAGE_BACKEND.md) |
| `DoubleBuffer<ExecutionPlan>` | [08](08_DOUBLE_BUFFER.md) |
| `DelayTask`, `DelayLine`, calcul PDC | [09](09_PDC.md) |
| Erreurs, diagnostic et limites | [10](10_ERREURS_DIAGNOSTICS.md) |
