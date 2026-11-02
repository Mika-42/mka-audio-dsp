# Sprint 8 — Compensation de latence

Les fiches S08-01 à S08-08 sont réalisées dans l'ordre, avec une PR par fiche vers `main`.

## S08-01 — Déclaration du Node

`LatencySamples` est un `uint32_t`. `Node::latencySamples() const noexcept` reste public, virtuel non pur, et renvoie zéro par défaut. La déclaration décrit le déplacement algorithmique des échantillons au sample rate effectif, commun à tous les chemins audio du Node en V1. Elle est indépendante du temps CPU, du nombre de frames et d'une queue sonore (`TailInfo`, hors champ).

Le contrôle capture la déclaration pour une génération. Le DSP et sa déclaration restent stables tant qu'un plan emprunte le Node : une adaptation de latence/fréquence demande la fin des emprunts ou un autre Node préparé pour B. Une simple publication atomique d'un nombre ne coordonne pas un changement de comportement DSP avec le swap. Le callback n'interroge pas cette méthode.

## S08-02 — Snapshot par génération

La préparation backend capture un `LatencySnapshot` possédé par le candidat : fréquence et table triée `NodeID → LatencySamples`. Le plan expose une vue `const`, invalide après remplacement/retrait sur contrôle. Une copie est indépendante. La compilation purement structurelle ne lit toujours aucune latence.

Le premier emprunt d'un Node ouvre une époque. La première préparation exclusive lit sa déclaration une fois et la fige sur contrôle ; les générations suivantes partageant ce Node recopient cette valeur sans interroger le DSP actif. Un changement de fréquence ou plusieurs emprunts non préparés sans déclaration figée renvoient `NodeInUse`. Le callback n'accède ni au snapshot ni au cache d'époque. La fin de tous les emprunts permet une nouvelle déclaration lors de la prochaine préparation. Pour changer la latence en activité, B doit utiliser un autre Node préparé indépendamment ; la migration d'état DSP n'est pas implicite.

Toutes les captures d'une génération réussissent avant de figer le cache. Le plan préparé n'est remplacé qu'après validation et allocation complètes. Les capacités de la table sont comptées dans son budget mémoire. Les tests incluent table indépendante, nouvelle époque, Pending, changement de fréquence refusé et capture concurrente avec le callback.
