# Stratégie de tests et critères d'acceptation

**Framework privilégié :** GoogleTest, assertions unitaires déterministes et doubles de backend, tests d'intégration ciblés. Priorité aux invariants structurels, puis à la mémoire, au RT, au handshake et au PDC. Ne pas mesurer la correction de la PDC avec un simple test « pas de crash » : la sortie doit être vérifiée sample à sample.

## A. DAG et connexions

| ID | Cas | Résultat attendu |
|---|---|---|
| G-01 | `Out(A,0)→In(B,0)` | Ajout réussi |
| G-02 | `In(B,0)→Out(A,0)` en sens inversé | Rejet `InvalidDirection` selon API |
| G-03 | `Out→Aux` | Accepté |
| G-04 | `In→In`, `Aux→Out`, `Out→Out` | Rejet |
| G-05 | Deux sources vers `In(B,0)` | Rejet `InputOccupied` |
| G-06 | Une source vers deux entrées différentes | Fan-out accepté |
| G-07 | Connexion vers canal absent du layout mais structurellement valide | Accepté |
| G-08 | Réinsertion même arête | Rejet doublon |
| G-09 | Boucle directe et cycle via 4 nœuds | Rejet `CycleDetected` |
| G-10 | Suppression d'un nœud, arêtes incidentes | Nettoyage correct, sous condition Node non référencé |
| G-11 | Modification de `outputsNodes` | Vues hardware sorties affectées à la bonne génération |

Exécuter tous les DAG possibles à 4 nœuds et toutes les arêtes candidates, en comparant un oracle indépendant. Utiliser des mutations aléatoires reproductibles avec graine et traces d'opération ; vérifier les transitions invalides comme les permutations open→start→stop→close côté backend seulement si l'intégration le nécessite.

## B. Buffers et contextes

| ID | Cas | Résultat attendu |
|---|---|---|
| B-01 | Mono/stéréo, `maxFrames=1/128/4096` | Slots exacts, pas de dépassement |
| B-02 | `frames` varie sur callbacks successifs | Uniquement `frames` samples écrits |
| B-03 | `frames > maxFrames` | Tout output backend à zéro, reconfiguration signalée |
| B-04 | Entrée interne sans connexion | Silence valide, jamais `nullptr` utilisé |
| B-05 | Deux consommateurs d'une même sortie | Slot vivant jusqu'à dernier consommateur |
| B-06 | Buffer terminé, réutilisé plus tard | Aucun signal encore lisible écrasé |
| B-07 | In-place sans compatibilité déclarée | Refus / slot distinct |
| B-08 | Historique FFT/Delay stocké dans Node | Non détruit à chaque callback |
| B-09 | PDC ajoute un lecteur et un producteur | `lastUse` tient compte des opérations PDC |
| B-10 | Capacité mémoire, produit overflow | `compile` échoue, plan A intact |

## C. Routage backend

| ID | Cas | Résultat attendu |
|---|---|---|
| IO-01 | Source avec `layoutInput={}` et 6 entrées backend | Le contexte reçoit 6 canaux |
| IO-02 | Source sélectionne entrées physiques 2 et 6 | Copie depuis indices 1 et 5 vers sorties locales 0/1 |
| IO-03 | Oscillateur sans connexion entrante | Peut ignorer tout `ctx.inputs` |
| IO-04 | Nœud devient non source après ajout d'une arête | Son `ctx.inputs` provient du DAG dans le nouveau plan |
| IO-05 | Adresse de backend change entre callbacks | Nouveaux pointeurs utilisés, jamais anciens |
| IO-06 | Deux sorties désignées vers casque et enceintes | Canaux 0/1 et 2/3 correctement écrits |
| IO-07 | Deux sorties écrivent même canal/samples | Dernière écriture dans ordre plan gagnante |
| IO-08 | Output non touché par aucun Node | Silence, pas de samples précédents |
| IO-09 | Plusieurs plans et retrait | Aucun Node ne conserve un pointeur backend obsolète |
| IO-10 | Descripteurs moins nombreux que backend réel | Silence sûr et diagnostic non RT |

## D. Publication et concurrence

| ID | Cas | Résultat attendu |
|---|---|---|
| SW-01 | A actif, B Ready → Pending | A exécute jusqu'à frontière de bloc |
| SW-02 | Pending observé par RT | B devient actif, ACK publié |
| SW-03 | Contrôle observe ACK | A nettoyé hors RT seulement |
| SW-04 | Deux publish avant ACK | Deuxième refusé/fusionné, aucune perte |
| SW-05 | Échec compilation B | A toujours actif |
| SW-06 | Node référencé par A puis B | Suppression bloquée jusqu'à extinction de toutes refs |
| SW-07 | Callback absent à l'arrêt | Reclamation sous quiescence explicite |
| SW-08 | Snapshot RT actif durant édition DAG | Aucune race de plan, de Node config ou de pool |

Instrumenter si possible AddressSanitizer/UndefinedBehaviorSanitizer et ThreadSanitizer sur harness contrôlé, avec prudence envers contraintes environnementales et bibliothèques plugins.

## E. Plugin Delay Compensation : oracle d'impulsions

Pour chaque test, injecter des impulsions au même instant logique et vérifier les **indices exacts** de restitution à la jonction et aux sorties prises en charge.

| ID | Graphe / condition | Attendu |
|---|---|---|
| P-01 | A latence 0, B latence 0 | Aucun delay ajouté |
| P-02 | A=64, B=256 vers Mix | Delay A=192, B=0 |
| P-03 | A=128 puis C=64, B=512 vers Mix | Delay chemin AC=320 |
| P-04 | Delay D=3, callback 4 frames | `[1,2,3,4]→[0,0,0,1]` |
| P-05 | Callback suivant | `[5,6,7,8]→[2,3,4,5]` |
| P-06 | D > frames, tailles 1/2/7/128 | Même sortie que l'oracle sample à sample |
| P-07 | Fan-out, retards différents selon consommateur | Source originale reste intacte |
| P-08 | Aux et In avec latences différentes | Compensation conforme à politique d'alignement audio |
| P-09 | Deux sorties marquées, latences chemin différentes | Alignement des chemins connus selon garantie documentée |
| P-10 | Dépassement de latence / mémoire | Erreur explicite, ancien plan intact |
| P-11 | Swap A→B avec nouveaux retards | Pas de UAF/data race, silence de warmup acceptable en V1 |
| P-12 | Plugin déclare mauvaise latence | Montrer explicitement qu'aucune mesure automatique ne corrige sa déclaration |

## F. Instrumentation RT

Mesurer : durée max et percentiles de callback, nombre de tâches, nombre de slots, `peakSlots`, mémoire de historiques PDC, latence cumulée max et longueur des delays, nombre de dépassements RT. Faire tourner un allocateur instrumenté / hooks du test pour détecter les allocations dans `exec`. Vérifier qu'aucun verrou ni appel de log/fichier n'est exécuté depuis le callback. Distinguer un benchmark local d'une preuve de deadline temps réel sur tout OS.

## G. Matrice de couverture

Chaque exigence du tableau dans [MATRICE_EXIGENCES](MATRICE_EXIGENCES.md) possède au moins un test. Les exigences « à arbitrer » ne deviennent pas acceptées par défaut ; écrire d'abord le contrat décidé, puis le test.
