# Sprint 9 — DelayLine et DelayTask persistantes

Les sept fiches S09-01 à S09-07 sont réalisées dans l'ordre, avec une branche et une PR par fiche vers `main`.

## S09-01 — DelayLine concrète

Le module `mka.audio.dsp.delay_line` expose `DelayLine final`, sans virtual ni Node. `prepare(uint64_t)` prépare hors RT un historique mono nul de D floats et remet le curseur à zéro. La surcharge avec `DelayLineLimits` vérifie un quota de samples et un budget d'historique inclusifs, illimités par défaut. Le plan contenant la ligne reste responsable du budget global des slots, métadonnées, silence et historiques. Conversion `size_t`, produit en octets et capacité adressable du conteneur sont vérifiés avant allocation.

La préparation construit un stockage temporaire avant commit ; tout refus de quota/taille ou `bad_alloc` préserve l'ancien historique et son curseur. `process(const float*,float*,FrameCount) noexcept` donne y[n]=x[n−D], avec x[n<0]=0, sans allocation, libération, mutex, attente, exception ou reset entre blocs. D est indépendant de maxFrames. frames=0 permet des pointeurs nuls et ne change aucun état.

Pour D>0 les plages source/destination sont disjointes ; le compilateur de plan doit prouver cette séparation par les slots et les bindings. D=0 passe directement ou copie, sans historique ; le compilateur ne matérialise pas de DelayTask. La source reste en lecture seule. Les copies dupliquent l'historique et le curseur ; les moves contrôlés transfèrent cet état et laissent une ligne D=0 valide. Préparer, copier, déplacer ou détruire une ligne active est interdit : ces opérations exigent la quiescence de son propriétaire audio.

## S09-02 — Oracle DelayLine isolé

`test_delay_line_oracle.cpp` importe seulement DelayLine, sans DAG ni backend. Il calcule chaque sortie par la formule absolue y[n]=x[n−D] avec zéro avant le début du flux ; aucun ring buffer ni helper de production ne sert de référence. Impulsions, rampes et alternances sont comparées sur 2 048 samples avec D=0/1/3/64/256/513/1025 et des blocs 0/1/2/7/128. Les mêmes flux sont rendus par sample, par blocs fixes et avec des partitions variables pour vérifier l'invariance à la segmentation.

Chaque callback contrôle l'intégralité de l'entrée read-only et les sentinelles de destination hors [0,frames). Mille blocs vides à pointeurs nuls ne changent ni historique ni destination. Les tests distinguent un reset explicite réussi d'un prepare refusé pour budget/overflow, et la préparation D=0 réussit même sous injection d'échec d'allocation. Les fenêtres RT incluent toutes les tailles de la matrice, sans allocation, libération ou mutex.

## S09-03 — DelayTask interne compilée

`detail::DelayTask` est un struct concret sans Node/virtual : source `const float*`, destination `float*`, D en `uint64_t`, et `DelayLine` possédée. `prepare` refuse D=0 (qui reste une liaison directe), prépare la ligne hors RT puis installe D. `bind` vérifie le compte préparé, les pointeurs nuls, l'arithmétique des plages et tout recouvrement, puis installe ses vues. Les champs/capacités ne changent plus en audio. `execute(FrameCount) noexcept` appelle seulement la ligne déjà prête.

ExecutionPlan utilise ce type pour ses opérations de retard ; l'ancien ring privé est supprimé. Le pool, les tâches et leurs lignes appartiennent à son stockage heap stable. Après copie contrôlée du pool, tous les pointeurs sont rebindés et validés ; déplacer le wrapper ne déplace pas ce stockage. Aucune adresse backend ne réside dans une DelayTask. Les historiques restent hors de la freelist et du buffer silence. Le budget total est vérifié avant la préparation des lignes ; tout refus de préparation/binding préserve l'ancien plan.

Les tests vérifient D=1/3/256 sur plusieurs callbacks, plages identiques/partiellement superposées/adjacentes, overflow des adresses sans déréférencement, refus préservant l'ancien flux, copie/move et rebinding de vrais plans, zéro frames/capacité, D=0 direct et zéro allocation/libération/mutex RT.

## S09-04 — Ordre final producteur → retard → consommateur

La compilation conserve un `PlanOperationInfo` immutable pour chaque opération finale : kind, index de tâche, NodeID pour les Nodes, connexion exacte et D sélectionné pour un retard, ainsi que ses identités de production avant recyclage. `operationSchedule()` expose cette vue numérique en lecture seule, jusqu'au remplacement/retrait du plan. Elle ne contient aucun pointeur DSP/backend et son stockage est inclus dans le budget.

Les DelayTasks positives sont insérées avant le consommateur, après leur producteur topologique. D=0 conserve la liaison directe. Les allocations du pool et des lignes finissent avant le binding ; les connexions et Nodes du DAG désiré restent inchangés. L'audio ne consulte que kind/index, sans construction de tâches ni résolution de ChannelID.

Les tests vérifient l'ordre et les samples sur une chaîne D=1+3+512, un diamant avec Aux retardé, un même Out vers trois jonctions D=1/3/512 et un lecteur direct tardif, des blocs variables et des guards. Un refus de B conserve le planning et l'historique actif. Les tests RT directs incluent D=512.
