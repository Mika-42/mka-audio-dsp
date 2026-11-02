# Sprint 10 — Validation V1

Sept fiches, sept branches/PR vers main, dans l'ordre.

## S10-01 — Recompilation de latence

Une adaptation prépare une **nouvelle instance DSP** immutable sur contrôle, retire l'ancienne du DAG désiré et raccorde la nouvelle avant `prepareAndPublish`. Les emprunts de A conservent l'ancien Node jusqu'à ACK/retrait. Les nœuds partagés conservent leur déclaration et fréquence figées ; `setConfig` refuse `NodeInUse`. Aucun prepare de plugin n'est exécuté en audio. Une notification audio doit seulement réveiller le contrôle via un mécanisme RT-safe fourni par l'adaptateur.

Les tests exécutent 0→64→256→0, deux canaux mono et un vrai gain/mix/routeur. Le snapshot, A/L/D et les historiques de B sont indépendants. Un budget refusé garde A ; une édition C pendant Pending ne modifie pas B. La suppression est différée et aucun destructeur de Node ne tourne en callback. Les histoires neuves peuvent produire un silence initial : V1 ne migre pas l'état et ne garantit pas de transition inaudible.

## S10-02 — Warmup autorisé au swap

Chaque DelayLine de B possède D floats nuls préparés hors RT. B ne lit ni ne copie les historiques de A : le contrôle ne récupère A qu'après ACK/quiescence. Le flux de la branche retardée de B suit y[n]=x[n−D], avec zéro pour n<D, où n commence à son premier sample traité, pas au premier callback (qui peut être vide). D=0 reste direct. Les états DSP des Nodes partagés ne sont pas réinitialisés ; seule la nouvelle compensation appartient à B. Le rendu total d'un Mix peut donc être partiellement audible pendant le warmup.

Le test rend une rampe après un A déjà chaud, pour D=0/3/256 et blocs variables 0/1/2/7. Il vérifie chaque sample et l'absence de reset sur les callbacks vides. Un refus mémoire laisse A intact. La discontinuité, le silence initial et un clic éventuel sont permis : aucune migration, préchauffe partagée ou transition sans clic n'est garantie en V1.

## S10-03 — Diagnostics contrôle

Les valeurs numériques ErrorCode 0..13 sont explicites et stables, avec les contextes NodeID/ChannelID existants. Le module optionnel `mka.audio.dsp.diagnostics` fournit `errorCodeName`, `formatError` et `formatCallbackDiagnostics`. Les formateurs peuvent allouer/lever et ne doivent être appelés que sur contrôle ; ils n'émettent eux-mêmes aucun log. Les valeurs/codes inconnus restent visibles.

PlanSwitcher agrège les bits atomiques relaxés après chaque callback et ajoute CallbackAdmissionRejected=64 pour un gate fermé/occupé. Les événements ne disparaissent plus lors du retrait d'un slot. Consommer via `takeCallbackDiagnostics()` sur contrôle, puis formater ; un événement concurrent au drain sera obtenu par ce drain ou le suivant. Le callback ne construit aucune chaîne. Les flags existants de frames, counts, pointeurs et capacités gardent leurs valeurs et le silence borné existant.
