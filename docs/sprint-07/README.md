# Sprint 07 — Deux générations, publication et retrait RT

Les huit fiches de l'export Trello utilisateur sont implémentées dans l'ordre,
sur des branches dépendantes visant main, sans fusion automatique ni changement
du tableau Trello.

| Fiche | Livrable |
|---|---|
| [S07-01](S07-01.md) | Deux emplacements ExecutionPlan stables et indépendants |
| [S07-02](S07-02.md) | Idle/Pending/Acknowledged, release/acquire et ownership |
| [S07-03](S07-03.md) | Swap en début de callback, jamais au milieu du bloc |
| [S07-04](S07-04.md) | ACK puis récupération de l'ancien plan sur contrôle |
| [S07-05](S07-05.md) | Éditions souhaitées et erreurs sans mutation de Pending |
| [S07-06](S07-06.md) | Nodes retirés conservés jusqu'à disparition de tous les borrows |
| [S07-07](S07-07.md) | Stop/join externe, annulation, reprise et shutdown |
| [S07-08](S07-08.md) | Stress à deux threads, oracle PDC et instrumentation |

Un seul contrôle sérialisé et un seul audio utilisent PlanSwitcher. Le DAG et les
owners des Nodes survivent au switcher. Les méthodes de contrôle ne modifient
jamais les états DSP partagés ; setConfig refuse un Node emprunté. Les vues Ready
expirent avant publication ; slotAddress ne donne qu'une identité stable et
n'autorise pas une lecture concurrente des samples/descripteurs actifs.

Le driver doit être stoppé et joint avant stopAfterBackendStopped, shutdown ou
destruction. Les contrôles d'admission ne prouvent pas à eux seuls qu'aucun
callback futur n'arrivera. Le switcher n'attend ni ACK ni mutex dans l'audio.
L'analyse automatique des latences et la migration sans clic des historiques
ne font pas partie de ce sprint ; les retards explicites sont préparés par plan.

Cycle d'intégration : initialiser avant démarrage du driver ; appeler exec(io)
sur audio ; éditer le DAG et appeler prepareAndPublish sur contrôle ; après ACK,
appeler retireAcknowledgedPlan puis collectRetiredNodes sur le DAG. À l'arrêt,
stop/join du driver précède stopAfterBackendStopped (conserve le dernier actif)
ou shutdownAfterBackendStopped (libère tout). Pour reprendre, appeler
resumeAfterBackendStopped avant de redémarrer le driver.

Validation : **305 tests passent avec GCC 16.2, Clang 19 et ASan/UBSan + détection
des fuites**. Les **17 scénarios PlanSwitcher** sont aussi validés avec TSan,
incluant 10 000 swaps concurrents. L'exécutable d'interception globale new/delete
est validé séparément en natif et ASan ; il n'est pas utilisé avec TSan.
Chaque fenêtre RT instrumentée relève zéro allocation, libération et mutex.
Doxygen : aucun avertissement.
