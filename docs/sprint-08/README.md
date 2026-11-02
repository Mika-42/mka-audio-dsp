# Sprint 8 — Compensation de latence

Les fiches S08-01 à S08-08 sont réalisées dans l'ordre, avec une PR par fiche vers `main`.

## S08-01 — Déclaration du Node

`LatencySamples` est un `uint32_t`. `Node::latencySamples() const noexcept` reste public, virtuel non pur, et renvoie zéro par défaut. La déclaration décrit le déplacement algorithmique des échantillons au sample rate effectif, commun à tous les chemins audio du Node en V1. Elle est indépendante du temps CPU, du nombre de frames et d'une queue sonore (`TailInfo`, hors champ).

Le contrôle capture la déclaration pour une génération. Le DSP et sa déclaration restent stables tant qu'un plan emprunte le Node : une adaptation de latence/fréquence demande la fin des emprunts ou un autre Node préparé pour B. Une simple publication atomique d'un nombre ne coordonne pas un changement de comportement DSP avec le swap. Le callback n'interroge pas cette méthode.
