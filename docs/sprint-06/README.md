# Sprint 06 — Routage matériel backend-agnostic

Exigences issues de l'export Trello fourni par l'utilisateur. Les huit fiches
sont traitées dans l'ordre, avec une branche et une PR par fiche vers main.
Le sprint 5 (PR #50 et ses prérequis) est conservé sans fusion automatique.

| Fiche | Livrable |
|---|---|
| [S06-01](S06-01.md) | Configuration backend fixe, Hz et conversions vérifiées |
| [S06-02](S06-02.md) | Sources définies par toutes les arêtes In/Aux |
| [S06-03](S06-03.md) | Inputs physiques complets, directs et const |
| [S06-04](S06-04.md) | Backend outputs directs et pool indépendant |
| [S06-05](S06-05.md) | Silence physique avant tout process |
| [S06-06](S06-06.md) | Last Writer Wins par sample |
| [S06-07](S06-07.md) | Routage simulé à deux et six sorties |
| [S06-08](S06-08.md) | Pipeline gain → deux paires et writer partiel |

Les branches sont en filiation : examiner/fusionner les prérequis du sprint 5,
puis S06-01 à S06-08 dans cet ordre. Les PR ultérieures incluent les commits
préalables tant qu'ils ne sont pas fusionnés. Le tableau Trello n'est pas modifié
et aucun passage métier à Done n'est implicite.

Migration : les comptes de voies du callback doivent correspondre exactement
au BackendConfig préparé, même à zéro frame. Un changement de forme requiert
une préparation inactive sur contrôle. Les sélections de voies et gains restent
dans le code des Nodes ; aucun ChannelID matériel, mapping de périphérique ou
mixage automatique n'est introduit.

Validation finale : **283/283 tests passent en Debug et sous ASan/UBSan avec
détection des fuites**. Les fenêtres RT de capacité, multi-source, silence,
trois writers, routage 2/6 et pipeline complet relèvent zéro new/delete/mutex.
Doxygen ne signale aucun avertissement. Ces vérifications utilisent un backend
simulé et ne nécessitent aucun matériel audio.
