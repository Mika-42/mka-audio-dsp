# Sprint 05 — Pool et recyclage

Source des exigences : export JSON Trello `x3aYznOU - mka-audio-dsp.json` fourni
par l'utilisateur. Fiches implémentées séquentiellement, sans modification du
tableau Trello et sans clôture/validation métier implicite.

| Fiche | Livrable | Branche |
|---|---|---|
| [S05-01](S05-01.md) | Slots mono float32 stables | `codex/s05-01-buffer-pool` |
| [S05-02](S05-02.md) | Silence const distinct | `codex/s05-02-readonly-silence` |
| [S05-03](S05-03.md) | producedAt / lastUse sur les opérations finales | `codex/s05-03-signal-liveness` |
| [S05-04](S05-04.md) | Réutilisation statique sans in-place | `codex/s05-04-static-slot-reuse` |
| [S05-05](S05-05.md) | Preuve de protection fan-out/output/Delay | `codex/s05-05-protect-fanout` |
| [S05-06](S05-06.md) | Budget, overflow et capacités | `codex/s05-06-checked-memory-budget` |
| [S05-07](S05-07.md) | Pipeline de préparation et binding stable | `codex/s05-07-compiler-pool-integration` |
| [S05-08](S05-08.md) | Oracle indépendant et instrumentation | `codex/s05-08-render-oracle` |

Les branches sont en filiation et visent `main`. Examiner/fusionner dans cet
ordre ; jusqu'à la fusion des prérequis, une PR ultérieure inclut leurs commits.
Aucune fusion automatique n'est exécutée.

Validation finale : **261/261 tests passent en Debug et sous ASan/UBSan**, avec
LeakSanitizer actif. Doxygen ne produit aucun avertissement. Les tests dédiés
comparent tous les samples intermédiaires et physiques à un interpréteur sans
recyclage ; le binaire RT intercepte new/delete et les acquisitions de mutex.

L'analyse automatique des latences, la gestion de publication RT et le routage
matériel détaillé appartiennent aux sprints suivants. Ici les retards mono
explicites fournis sur contrôle sont exécutés et intégrés au calcul des vies,
du budget et des slots, sans historique dans la freelist.
