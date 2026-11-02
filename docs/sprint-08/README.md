# Sprint 8 — Compensation de latence

Les fiches S08-01 à S08-08 sont réalisées dans l'ordre, avec une PR par fiche vers `main`.

## S08-01 — Déclaration du Node

`LatencySamples` est un `uint32_t`. `Node::latencySamples() const noexcept` reste public, virtuel non pur, et renvoie zéro par défaut. La déclaration décrit le déplacement algorithmique des échantillons au sample rate effectif, commun à tous les chemins audio du Node en V1. Elle est indépendante du temps CPU, du nombre de frames et d'une queue sonore (`TailInfo`, hors champ).

Le contrôle capture la déclaration pour une génération. Le DSP et sa déclaration restent stables tant qu'un plan emprunte le Node : une adaptation de latence/fréquence demande la fin des emprunts ou un autre Node préparé pour B. Une simple publication atomique d'un nombre ne coordonne pas un changement de comportement DSP avec le swap. Le callback n'interroge pas cette méthode.

## S08-02 — Snapshot par génération

La préparation backend capture un `LatencySnapshot` possédé par le candidat : fréquence et table triée `NodeID → LatencySamples`. Le plan expose une vue `const`, invalide après remplacement/retrait sur contrôle. Une copie est indépendante. La compilation purement structurelle ne lit toujours aucune latence.

Le premier emprunt d'un Node ouvre une époque. La première préparation exclusive lit sa déclaration une fois et la fige sur contrôle ; les générations suivantes partageant ce Node recopient cette valeur sans interroger le DSP actif. Un changement de fréquence ou plusieurs emprunts non préparés sans déclaration figée renvoient `NodeInUse`. Le callback n'accède ni au snapshot ni au cache d'époque. La fin de tous les emprunts permet une nouvelle déclaration lors de la prochaine préparation. Pour changer la latence en activité, B doit utiliser un autre Node préparé indépendamment ; la migration d'état DSP n'est pas implicite.

Toutes les captures d'une génération réussissent avant de figer le cache. Le plan préparé n'est remplacé qu'après validation et allocation complètes. Les capacités de la table sont comptées dans son budget mémoire. Les tests incluent table indépendante, nouvelle époque, Pending, changement de fréquence refusé et capture concurrente avec le callback.

## S08-03 — Temps cumulés

`PdcAnalysis` contient, dans l'ordre topologique de la génération, les valeurs `P` (`uint32_t`), `A` et `L` (`uint64_t`) de chaque NodeID. `A(v)=max L(u)` sur ses prédécesseurs audio, ou zéro s'il n'y en a aucun ; `L(v)=checkedLatencyAdd(A(v),P(v))`. L'analyse pure ne possède et ne consulte aucun Node. Elle lit uniquement le snapshot et les identités numériques des connexions audio.

La préparation retient comme arête audio une sortie Out présente dans `layoutOutput` dont la destination est présente dans `layoutInput` ou `layoutAuxiliary`. Les ports hors Config ne créent pas de lecteur : leurs liens continuent pourtant de contraindre l'ordre structurel. Une source logique non produite reste silencieuse. Aucun historique ni plan A n'est modifié par l'analyse. Les tables sont possédées et comptées au budget de B, sans accès dans `exec`.

Tests : sources 0/3/8/512, chaîne, diamant In/Aux, ordre indépendant du stockage, ports ignorés, cumuls au-delà de 32 bits, erreurs de table/ordre/ports et addition proche de `UINT64_MAX`. Les limites 64 bits sont testées avec des opérandes cumulés synthétiques pour éviter de construire un graphe gigantesque.

## S08-04 — Retards par connexion

L'analyse possède maintenant une table complète `Connection{from,to} → D` triée par destination puis source. Pour chaque arête audio, `D=A(destination)-L(source)` est non négatif en `uint64_t`. La latence propre du consommateur est ajoutée ensuite, une fois. Les délais positifs insèrent une opération interne avant le consommateur, avec sortie et historique indépendants. Un même Out peut donc alimenter deux jonctions avec des délais différents. `D=0` garde le pointeur direct, sans DelayTask, y compris dans un planning explicite.

La compilation backend applique automatiquement les D calculés. L'ancien champ `BackendConfig::connectionDelays`, s'il est non vide, fournit un planning explicite complet qui remplace l'insertion automatique (pour les intégrations existantes) ; les tables P/A/L/D restent l'analyse mathématique des déclarations intrinsèques, pas une description de ce planning manuel. Le span n'est jamais conservé. Dans les deux cas les opérations finales sont soumises à la même preuve de liveness, au budget et à la transaction de préparation.

Les historiques internes appartiennent au plan, ne modifient pas le DAG, et traversent les callbacks. Les tests vérifient 3/8 → 5/0, Aux, pointeurs directs, fan-out à retards différents, impulsions sur deux blocs et latence du consommateur. L'exécutable RT instrumente une compensation automatique de 256 samples avec maxFrames=128 : zéro allocation, libération et mutex dans les callbacks.

## S08-05 — Fan-out, Aux et lecteurs réels

Les DelayTasks lisent la production Out commune sans la modifier et écrivent une nouvelle production logique. La preuve de liveness porte sur la séquence finale Node+Delay : le signal source vit jusqu'au dernier lecteur, qu'il soit un Node In/Aux direct ou une DelayTask. Les historiques des branches restent indépendants. Les slots PCM des temporaires peuvent être recyclés lorsque leurs intervalles ne se chevauchent pas ; cela ne partage jamais les historiques.

Un Aux déclaré et produit participe au maximum et reçoit son D exact comme un In. Une destination hors Config n'est pas un lecteur audio. Une sortie absente de layoutOutput ne produit aucun signal : son consommateur déclaré lit le silence, sans compensation artificielle. Une entrée non connectée reste aussi silencieuse. La contrainte topologique et le statut structurel de source restent déterminés par toutes les connexions désirées.

Les tests rendent un diamant et dix jonctions à délais différents, en stratégies Reuse et Unique, avec des blocs 0/1/2/3/4. Ils comparent chaque échantillon In/Aux et produit à une rampe décalée calculée indépendamment, puis vérifient qu'un lecteur direct placé après les dix branches retrouve la source intacte. Les identités du DAG restent inchangées : aucun Node PDC ou mélangeur implicite.

## S08-06 — Sorties logiques et routage matériel libre

Le PDC V1 compense uniquement les arêtes logiques connues, avant le process consommateur. Un marqueur output ne décrit aucune voie matérielle : le Node reçoit directement tous les `backendOutputs` et choisit ses écritures. L'analyse n'invente ni mapping par indice, ni retard terminal global, ni compensation des transformations internes arbitraires du dernier Node. Cette limite est aussi documentée dans l'API Doxygen de `PdcAnalysis`.

Pour synchroniser deux destinations matérielles, réunir d'abord les branches dans une jonction Mix/routeur dont les In/Aux sont compensés, puis router son signal commun vers les voies souhaitées. Deux Nodes output indépendants à 3 et 8 samples gardent leurs propres temps physiques. Le moteur n'en déduit pas un alignement automatique par canal.

Les tests rendent la jonction 3/8 vers les voies libres 2 et 5, les deux outputs indépendants sans retard artificiel, un routeur output avec successeur (échantillons logiques distincts de ses écritures physiques) et plusieurs writers dont les plages d'écriture se recouvrent. Le dernier writer gagne par échantillon, sans sommation implicite.

## S08-07 — Limites, budget et transaction

`BackendConfig::maxDelaySamples` limite chaque historique en samples, indépendamment de `maxFrames` ; `memoryBudgetBytes` limite le total du plan. Les deux quotas sont inclusifs et illimités par défaut, sans plafond arbitraire. `checkedDelayBytes` vérifie le quota, la conversion en `size_t` et le produit par `sizeof(float)` avant allocation. La préparation vérifie aussi la capacité adressable du conteneur ; l'addition des historiques, slots temporaires, silence, tables P/A/L/D, autres métadonnées et padding utilise l'arithmétique contrôlée du budget total.

Un dépassement de quota retourne `MemoryBudgetExceeded`, une taille/arithmétique non représentable `ArithmeticOverflow`, un cumul temporel non représentable `LatencyOverflow`, et un `bad_alloc` de préparation `PrepareFailed`. Les erreurs de retard portent le NodeID et ChannelID destination. Toutes surviennent sur contrôle, avant publication ; la transaction ne remplace jamais A par un état partiel.

Tests : D=0/1/256 avec maxFrames=4, plusieurs historiques, budget exact et un octet en dessous, limite de retard exacte et trop courte, limites `size_t`/`uint64_t` sans allocation gigantesque. Deux refus de B pendant un rendu continu doivent laisser A à sa génération et poursuivre exactement son historique. L'exécutable d'injection échoue à chaque point d'allocation d'une préparation PDC automatique, puis vérifie les tables/pointeurs et les échantillons de A contre une rampe retardée indépendante. Il ne rend pas une copie de plan partageant les mêmes historiques DSP de Node.
