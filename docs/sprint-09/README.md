# Sprint 9 — DelayLine et DelayTask persistantes

Les sept fiches S09-01 à S09-07 sont réalisées dans l'ordre, avec une branche et une PR par fiche vers `main`.

## S09-01 — DelayLine concrète

Le module `mka.audio.dsp.delay_line` expose `DelayLine final`, sans virtual ni Node. `prepare(uint64_t)` prépare hors RT un historique mono nul de D floats et remet le curseur à zéro. La surcharge avec `DelayLineLimits` vérifie un quota de samples et un budget d'historique inclusifs, illimités par défaut. Le plan contenant la ligne reste responsable du budget global des slots, métadonnées, silence et historiques. Conversion `size_t`, produit en octets et capacité adressable du conteneur sont vérifiés avant allocation.

La préparation construit un stockage temporaire avant commit ; tout refus de quota/taille ou `bad_alloc` préserve l'ancien historique et son curseur. `process(const float*,float*,FrameCount) noexcept` donne y[n]=x[n−D], avec x[n<0]=0, sans allocation, libération, mutex, attente, exception ou reset entre blocs. D est indépendant de maxFrames. frames=0 permet des pointeurs nuls et ne change aucun état.

Pour D>0 les plages source/destination sont disjointes ; le compilateur de plan doit prouver cette séparation par les slots et les bindings. D=0 passe directement ou copie, sans historique ; le compilateur ne matérialise pas de DelayTask. La source reste en lecture seule. Les copies dupliquent l'historique et le curseur ; les moves contrôlés transfèrent cet état et laissent une ligne D=0 valide. Préparer, copier, déplacer ou détruire une ligne active est interdit : ces opérations exigent la quiescence de son propriétaire audio.
