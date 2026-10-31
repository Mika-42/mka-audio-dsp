# Algorithme — Publication, swap, retrait et destruction

## Cycle nominal

```mermaid
sequenceDiagram
 autonumber
 participant UI as UI contrôle
 participant DAG
 participant DB as DoubleBuffer A/B
 participant RT as RT callback
 participant N as Node
 UI->>DAG: éditer état souhaité
 UI->>DAG: compile(back B)
 DAG-->>DB: B Ready, Node* empruntés
 UI->>DB: publish B => Pending release
 RT->>DB: au début d'un callback, load acquire
 RT->>DB: adopter B, A devient Retired
 RT->>DB: store Acknowledged release
 RT->>N: B.tasks[].process()
 UI->>DB: load Acknowledged acquire
 UI->>DB: releaseRetired(A) hors RT
 UI->>DB: Idle, slot A libre
```

Un `Pending` ne doit jamais déclencher une attente RT. Les deux plans gardent leur mémoire propre, y compris pools et delay lines. **Seuls les `Node*` peuvent être partagés**, sous réserve de la vie des objets et de la coordination des paramètres.

## Automate

```mermaid
stateDiagram-v2
 [*] --> Free
 Free --> Ready: compilation complète
 Ready --> Pending: publish
 Pending --> Active: RT block boundary
 Active --> Retired: next successful swap
 Retired --> Free: control retire after ACK
 Ready --> Free: abandon if unpublished
```

Le drapeau d'échange externe vaut `Idle/Pending/Acknowledged`, tandis que les plans occupent leurs rôles `Free/Ready/Pending/Active/Retired` selon la génération. L'ordre atomique release/acquire synchronise la publication du contenu non atomique du plan B et la reconnaissance de son abandon par RT.

## Cas d'échec compilatoire

```mermaid
flowchart TD
 A[Plan A actif] --> B[Editer DAG souhaité]
 B --> C[Compiler B]
 C --> D{B complètement préparé ?}
 D -->|non| E[Garder A actif + retourner erreur]
 D -->|oui| F[Publier B Pending]
 F --> G[RT adopte B]
 G --> H[ACK puis nettoyage de A]
```

Ne pas écraser A pour « corriger » une ressource manquante ; le DAG souhaité peut rester plus récent que le plan réellement actif. Plusieurs éditions successives peuvent être fusionnées tant que B n'est pas publié.

## Cas suppression

Suppression physique d'un Node après preuve qu'il n'est référencé par **aucun** des deux plans. Un marquage de suppression souhaitée puis compilation d'une génération sans ce Node permet de conserver A sans crash jusqu'au swap. Après ACK, nettoyer A, vérifier B et toute autre référence, puis détruire le Node sur contrôle. Si cette stratégie n'est pas encore implémentée, `remove(id)` renvoie `NodeInUse` et laisse l'objet vivant.

```mermaid
flowchart TD
 A[Demande de suppression NodeID] --> B{Un plan référence Node ?}
 B -->|non| C[Supprimer arêtes/marks + détruire Node hors RT]
 B -->|oui| D[Retirer logiquement du DAG souhaité]
 D --> E[Compiler et publier un plan sans Node]
 E --> F{ACK reçu et autres refs absentes ?}
 F -->|non| G[Conserver Node ou renvoyer NodeInUse]
 F -->|oui| C
```

## Plan actif avec PDC

Lors du swap A→B, l'historique des DelayTask B est neuf (par défaut tout zéro). Cela peut créer une rupture audible ; V1 n'exige pas de migration sans clic. L'état du plugin partagé doit être compatible avec la latence attendue par B au moment de l'activation, sans mutation de paramètres non atomiques par contrôle.

## Stop backend

Si le backend est arrêté, le callback peut ne plus venir reconnaître Pending. Une preuve externe de quiescence (backend confirmé hors callback) peut autoriser le contrôle à terminer un changement, vider Pending ou détruire tous les plans, mais **pas** en simulant un ACK pendant qu'un callback serait encore en cours. Procédure exacte à documenter dans l'intégration de la couche 0.
