# Questions ouvertes et garde-fous avant implémentation

**Lecture importante :** les décisions fonctionnelles V1 sont validées ; les points ci-dessous concernent des **précisions nécessaires à une implémentation sans data race et sans pointeur invalide**. Les combler sans le dire reviendrait à inventer une décision.

| ID | Question | Ce qui est déjà décidé | Risque si ignoré | Proposition prudente (non validée) |
|---|---|---|---|---|
| O-01 | Un nœud marqué sortie peut-il également avoir des connexions sortantes ? | `outputsNodes` donne accès à tous les buffers matériels. | Le même `ctx.outputs` ne peut décrire simultanément `BufferPool` et backend entier. | Exiger nœud terminal pour la V1, ou exposer une sortie interne séparée. |
| O-02 | Nœud source **et** sortie désignée ? | Source voit tous `inputs`, sortie voit tous `outputs`. | Un `Node` pourrait router directement input→hardware sans buffers intermédiaires ; PDC ne saurait pas tout contrôler. | Autoriser explicitement mais exclure des garanties PDC complexes, ou interdire par validation. |
| O-03 | Quel type exact pour `Channel.buffer` ? | Inputs backend `const`, outputs modifiables. | Avec un `float*` unique, l'API ne garantit pas lecture seule. | `InputChannel{const float*}` + `OutputChannel{float*}` ; conserver `ProcessContext.inputs` et `.outputs`. |
| O-04 | Que vaut le champ `ChannelID` des canaux backend externes ? | Tous canaux exposés par indice de backend ; pas de `ChannelID` matériel spécial validé. | Encodage d'un `NodeID` fictif non prévu. | Distinguer identifiant logique d'un index externe, ou exposer descripteur backend sans ChannelID pour les sources/sinks. |
| O-05 | Stabilité des adresses après `compile()` ? | Descripteurs et buffers ne peuvent se déplacer après publication. | Vues internes pendantes si `vector` réalloué/moved. | Préallouer et fixer stockage final ; ne pas déplacer un plan Ready/Active ; tests de références. |
| O-06 | Suppression physique d'un Node déjà actif | Pas de destruction si référencé par A ou B. | UAF, ou impossible de retirer un Node à chaud. | `requestRemove` dans graphe souhaité puis `collect` après ACK, au lieu d'un `remove` monolithique. |
| O-07 | Où est préparé/configuré l'état d'un plugin partagé entre A/B ? | RT est seul à exécuter Node ; PDC plan B préparé hors RT. | Une mise à jour du plugin partagé peut modifier A pendant `process`. | Snapshots de paramètres, versions d'états ou activation synchronisée au swap. |
| O-08 | Latence scalaire d'un nœud multi-entrées | V1 suppose un retard déclaré par Node. | Les chemins internes/ports peuvent avoir des décalages différents non modélisés. | Exiger contrat de latence uniforme ; considérer latences par port pour V2. |
| O-09 | Port auxiliaire et alignement PDC | `Aux` est un port audio, peut nécessiter alignement. | Certains plugins s'attendent à un sidechain volontairement non aligné. | Synchroniser par défaut les signaux audio des ports utilisés ; prévoir choix explicite ultérieur. |
| O-10 | Connexion structurelle vers canal non déclaré | Autorisée. | Si la source ne produit pas ce canal, un consommateur peut lire un buffer non initialisé. | `compile` garantit silence sur productions non matérialisées/écrites ; ne jamais laisser un input lecteur `nullptr`. |
| O-11 | Garantie PDC avec sortie matérielle routée librement | Tous nœuds de sortie peuvent écrire partout, LWW. | Compilateur ne connaît pas quel canal hardware reçoit quel chemin. | Aligner chemins vers nodes de sortie connus, documenter absence de preuve par canal. |
| O-12 | Ordre topologique stable | LWW dépend du dernier exécuté. | Effets audibles changeant sans changement logique lors d'une recompilation. | Tri stable par insertion/ID, ordre d'exécution explicite et test de stabilité. |
| O-13 | Limites `maxFrames`, canaux et mémoire | Pas de débordement, sortie sûre et reconfiguration hors RT. | Plan actif invalidé par reconfiguration backend. | `CompileConfig` avec capacités explicites, fail-safe silence si callback hors contrat. |
| O-14 | Silence à capacité zéro ou callback frames=0 | Pas de buffer invalide. | `process` de plugins peut ne pas gérer frames 0. | Gérer frames 0 comme retour sans traitement après effacement requis. |
| O-15 | Budget mémoire : pool + delay lines | Pas d'allocation RT. | PDC pourrait dépasser le budget si pool compté seul. | Un budget total agrégé contrôlé pendant `compile`, heuristiques de headroom secondaires. |
| O-16 | In-place multicanal | Accepté seulement sous conditions. | Cross-channel aliasing et écrasement de lectures futures. | Contrat par paire de canaux, activation après tests. |
| O-17 | Backend arrêté pendant Pending | RT ne fera pas forcément un nouveau callback. | Slot Pending bloqué, fuite de ressources ou détruit trop tôt. | Nettoyage après quiescence backend prouvée. |
| O-18 | Historiques DelayLine au swap | V1 ne garantit pas transition inaudible. | Clic, samples manquants pendant warmup. | Documenter comportement et envisager migration ou fondu V2. |
| O-19 | `TailInfo` exact | Métadonnée maintenue, drainage audible hors V1. | Interprétation erronée de tail comme latency. | Fiche/enum séparés pour durée de queue, sans `DelayTask` automatique. |
| O-20 | Retour d'erreur exact de `Node::prepare()` | Échec possible en compilation selon discussions. | Exception/erreur à la mauvaise étape, Node partagé modifié. | Spécifier `prepare` non RT ou usine de ressources de plan, traiter erreur `PrepareFailed`. |
| O-21 | Nom `NodeID` dans Excalidraw : u64 vs encodage 32 bits | ChannelID = 32 +16 +16. | Troncature implicite de NodeID. | API NodeID uint32 avec conversion explicite si ancienne API utilise uint64. |
| O-22 | Intégration `AudioProcessContext` source réelle | Backend layer zero impose normalized `float` channels et frames. | Exemple doc non identique à l'API du dépôt. | Adapter les structures de référence à `mka::audio::AudioProcessContext` réel sans changer le contrat. |

## Comment trancher sans rouvrir toute l'architecture

Pour chaque point, ajouter une ADR courte : **problème → invariants existants → options → choix → conséquences → tests**. Quand une recommandation impose une règle supplémentaire (par exemple « sortie marquée doit être terminale »), la noter comme **nouveau choix à valider**, jamais comme « déjà décidé ».

## Note spécifique à la compensation et aux nœuds sources

Une source topologique reçoit tout le backend, même si `layoutInput` est vide. Si son `process` génère de l'audio sans lire les canaux externes, il peut avoir une latence intrinsèque propre. Le calcul PDC utilise sa déclaration `latencySamples`. Dans un graphe où les entrées Aux ne font qu'une influence de contrôle non alignée intentionnellement, un PDC uniforme sur tous les prédécesseurs peut apporter un décalage inattendu : cette limite doit être explicitée dans le contrat du processeur.

## Note spécifique au « double buffer » de la DelayLine

Le terme « double buffer » a été évoqué intuitivement pendant le brainstorming PDC. La réalisation retenue pour le retard est **une mémoire circulaire persistante** de D samples par chemin (ou une structure équivalente). Le `DoubleBuffer<ExecutionPlan>` n'est **pas** cette mémoire. Il sert uniquement à publier/retirer en sûreté deux générations du moteur.
