# Sprint 10 — Validation V1

Sept fiches, sept branches/PR vers main, dans l'ordre.

## S10-01 — Recompilation de latence

Une adaptation prépare une **nouvelle instance DSP** immutable sur contrôle, retire l'ancienne du DAG désiré et raccorde la nouvelle avant `prepareAndPublish`. Les emprunts de A conservent l'ancien Node jusqu'à ACK/retrait. Les nœuds partagés conservent leur déclaration et fréquence figées ; `setConfig` refuse `NodeInUse`. Aucun prepare de plugin n'est exécuté en audio. Une notification audio doit seulement réveiller le contrôle via un mécanisme RT-safe fourni par l'adaptateur.

Les tests exécutent 0→64→256→0, deux canaux mono et un vrai gain/mix/routeur. Le snapshot, A/L/D et les historiques de B sont indépendants. Un budget refusé garde A ; une édition C pendant Pending ne modifie pas B. La suppression est différée et aucun destructeur de Node ne tourne en callback. Les histoires neuves peuvent produire un silence initial : V1 ne migre pas l'état et ne garantit pas de transition inaudible.
