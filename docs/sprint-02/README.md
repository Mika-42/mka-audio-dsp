# Sprint 02 — DAG éditable

Source : export JSON Trello fourni par l’utilisateur. Le numéro de fiche dans
le sprint est distinct du numéro global Trello. Une seule fiche est livrée à
la fois. L’utilisateur a validé les fiches 1 à 3 puis autorisé les fiches 4 à 8
séquentiellement, avec une PR distincte vers main pour chacune, sans fusion.

| Fiche du sprint | Trello | Référence | Livrable | État |
|---|---|---|---|---|
| 1 | [#9](https://trello.com/c/tfBlFRbF) | S02-01 | Stocker des Node avec adresses stables dans DAG | Validée par l’utilisateur |
| 2 | [#10](https://trello.com/c/Ds9AYM8Q) | S02-02 | Définir Connection et stocker les arêtes | Validée par l’utilisateur |
| 3 | [#11](https://trello.com/c/QLYlqIUR) | S02-03 | Implémenter connect : sens des ports et existence des nodes | Validée par l’utilisateur |
| 4 | [#12](https://trello.com/c/YtF0Vv5A) | S02-04 | Interdire la double occupation d’un canal entrant | Implémentée, en attente de validation |
| 5 | [#13](https://trello.com/c/YyfwJ1r3) | S02-05 | Implémenter disconnect précis et idempotence documentée | Non abordée |
| 6 | [#14](https://trello.com/c/0ZpwaMB7) | S02-06 | Supprimer un nœud et nettoyer ses arêtes | Non abordée |
| 7 | [#15](https://trello.com/c/8MYAy6PM) | S02-07 | Gérer markAsOutput et la collection outputsNodes | Non abordée |
| 8 | [#16](https://trello.com/c/fgNXIV95) | S02-08 | Tester un petit DAG éditable de bout en bout | Non abordée |

Voir [S02-01](S02-01.md) pour le contrat livré, la migration d’API et les tests.
Les valeurs d’arêtes figurent dans [S02-02](S02-02.md). La fiche en cours est
[S02-04](S02-04.md), consacrée à l’occupation des canaux entrants.
Le stockage de retrait nécessaire à S02-01 ne vaut pas validation de S02-06 et
de ses scénarios de publication à venir. Le tableau Trello et son export ne
sont pas modifiés. Le [sprint 01](../sprint-01/README.md) est validé par le
passage au sprint 2 demandé dans la conversation.
