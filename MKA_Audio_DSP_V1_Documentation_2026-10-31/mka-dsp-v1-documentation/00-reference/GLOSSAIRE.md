# Glossaire pédagogique

## Audio numérique

| Terme | Définition et exemple |
|---|---|
| **Échantillon / sample** | Une valeur numérique de l'amplitude d'un signal à un instant discret. Ici un `float32` par canal. |
| **Sample rate / fréquence d'échantillonnage** | Nombre d'échantillons par seconde et par canal, ex. 48 000 Hz. |
| **Frame audio** | Ensemble des échantillons au même instant sur tous les canaux. 128 frames stéréo = 128 samples sur L + 128 sur R. |
| **Bloc / callback** | Appel du backend fournissant `frames` frames à traiter. La taille effective peut varier selon l'appel. |
| **Canal** | Flux mono indépendant, ex. entrée 5 d'une interface. La stéréo est représentée par deux canaux. |
| **Planar** | Un pointeur `float*` distinct par canal, contigu sur les `frames` de ce canal. |
| **Backend** | Adaptateur ALSA, JACK, PipeWire, WASAPI, CoreAudio, etc. ; il fournit les buffers et lance le callback. |
| **Endpoint** | Point d'entrée/sortie exposé par la couche backend, avec format, fréquence, taille et nombre de canaux. |
| **DSP** | Digital Signal Processing : calculs appliqués aux signaux audio. |
| **Lookahead** | Retarder le chemin audio pour analyser des échantillons en avance par rapport à ceux restitués. |
| **Latence algorithmique** | Décalage, en samples, introduit par un processeur. Différente du coût CPU d'un callback. |
| **PDC** | Plugin Delay Compensation : retarde les chemins rapides pour les aligner sur les plus lents. |
| **Tail** | Persistance d'une sortie sonore après l'arrêt de son entrée (ex. réverbération). Ne désigne pas nécessairement une latence. |
| **Delay line** | Mémoire circulaire qui restitue `x[n-D]` avec un retard `D` échantillons. |
| **Mix** | Somme explicite de plusieurs signaux. Sans normalisation implicite, la somme peut dépasser ±1 en float. |
| **Split** | Duplication explicite d'une entrée vers plusieurs sorties. Ne signifie pas automatiquement un partage mutable. |
| **Sidechain / Aux** | Entrée auxiliaire utilisée, par exemple, par le détecteur d'un compresseur. |
| **In-place** | Un processeur travaille avec une entrée et une sortie qui partagent une même zone mémoire, uniquement si le contrat d'aliasing le permet. |
| **Silence buffer** | Zone de zéros réutilisable en lecture seule pour les entrées internes non connectées. |

## Graphes

| Terme | Définition |
|---|---|
| **Nœud / Node** | Processeur identifié par un ID stable, doté d'une configuration et d'un `process()` virtuel. |
| **DAG** | Graphe orienté acyclique : les connexions suivent un sens et aucun chemin ne revient sur son origine. |
| **Arête / Connection** | Liaison dirigée entre un canal `Out` producteur et un canal `In` ou `Aux` consommateur. |
| **Port** | Catégorie de canal, ici `In`, `Out`, `Aux`. |
| **ChannelID** | Identité logique encodant `NodeID`, type de port et indice de canal. Ne porte pas de pointeur audio. |
| **Source topologique** | Nœud sans arête entrante (ni `In`, ni `Aux`) dans la version du graphe compilée. Peut ignorer les entrées du backend. |
| **Sortie désignée** | Nœud référencé dans `outputsNodes`, autorisé à accéder aux sorties du backend. Différent de « sans successeur ». |
| **Tri topologique** | Ordre de visite plaçant tout producteur avant ses consommateurs. Plusieurs ordres valides peuvent exister : fixer un ordre stable pour les écritures LWW. |
| **Fan-out** | Une sortie alimente plusieurs consommateurs. |
| **Fan-in** | Plusieurs contributions convergent vers un traitement de mixage explicite, via plusieurs canaux d'entrée distincts. |
| **Last-use / liveness** | Dernier instant du plan où un buffer doit encore être lisible. Permet son recyclage après cette utilisation. |
| **Slot** | Emplacement mono physique alloué au pool ; un slot contient `maxFrames` `float` au maximum. |

## C++ et concurrence

| Terme | Définition |
|---|---|
| **Thread RT / audio** | Thread qui exécute le callback ; ne doit ni attendre, ni allouer, ni prendre de verrou bloquant. |
| **Thread de contrôle** | Thread non temps réel qui édite le DAG, compile et publie des plans. |
| **Ownership / propriétaire** | Objet responsable de la durée de vie et de la destruction de la mémoire. |
| **Vue non propriétaire** | Pointeur/span qui donne accès à une mémoire sans en être propriétaire ; sa validité dépend du propriétaire. |
| **`const float*`** | Permet de lire les valeurs mais pas de les modifier par ce pointeur. Les pointeurs backend entrants sont contractuellement en lecture seule. |
| **`float*`** | Pointeur permettant d'écrire ; les buffers de sortie backend et du pool en sont des exemples. |
| **`std::span`** | Vue C++ contenant adresse + longueur ; les éléments ne lui appartiennent pas. |
| **`std::expected<T,E>`** | Résultat hors RT contenant une valeur ou une erreur décrite par `E`. |
| **`noexcept`** | Garantie C++ déclarative qu'une fonction ne laisse pas sortir d'exception. Ne prouve pas l'absence d'allocation ou de blocage. |
| **`std::atomic`** | Accès synchronisé à une petite variable partagée entre threads sans data race sur cet objet. |
| **Release/acquire** | Paire d'ordres mémoire qui publie puis rend visibles des écritures non atomiques réalisées avant la publication. |
| **Data race** | Accès concurrents non synchronisés au même emplacement dont au moins un est une écriture : comportement indéfini. |
| **Handshake** | Protocole de confirmation entre thread de contrôle et thread RT pour publier et retirer les plans. |
| **Plan actif** | Plan exécuté par le thread audio ; jamais recompilé tant qu'il est actif. |
| **Plan retraité/retired** | Ancien plan que le RT ne référence plus et que le contrôle peut ensuite nettoyer. |
| **Double buffering de plans** | Deux objets `ExecutionPlan` alternent actif/préparé. **À ne pas confondre** avec une delay line PDC, qui est une mémoire circulaire. |
| **ABA / génération** | Problème potentiel si un identifiant/adresse est réutilisé alors qu'une ancienne référence survit. Ici la destruction des nœuds attend la libération de tous les plans. |

## Unités et notations

`NodeID` : identifiant de nœud, type public illustratif `std::uint32_t` (l'ancien schéma note parfois `u64` ; l'encodage `ChannelID` réserve 32 bits). `ChannelIndex` : `std::uint16_t`. `BufferId` / `TaskIndex` : `std::uint32_t` ou `std::size_t`, à fixer de façon uniforme. `frames` : taille effective par callback, `0 < frames <= maxFrames` en chemin nominal. `maxFrames` : capacité préparée. `D` : retard PDC entier, en samples ; `P(v)` : latence déclarée du nœud ; `L(v)` : latence cumulée à sa sortie. Les indices C++ de canaux commencent à zéro, même lorsque l'appareil affiche ses prises « 1, 2, 3… ».
