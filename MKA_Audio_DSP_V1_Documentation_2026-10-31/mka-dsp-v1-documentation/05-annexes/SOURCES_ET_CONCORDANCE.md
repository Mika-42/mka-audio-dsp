# Sources examinées et concordance

## Documents fournis dans l'environnement de travail

1. **Discussions MKA Audio DSP**, septembre–31 octobre 2026 : fondations du DAG, connexions, buffers, publication, entrées/sorties backend et PDC. Les dernières validations explicites ont priorité sur les hypothèses exploratoires antérieures.
2. **`Journal_decisions_mka_audio_DSP_2026-10-25.md`** : consignation des choix topologie, tasks, pool, last-use, in-place, frames variables, suppression sûre et exclusions initiales. Le document date du **25 octobre**, donc sa mention « la latence avancée est à préciser » est remplacée par l'ajout explicite du **PDC en V1** fin octobre.
3. **`Texte collé(20261031-080901).txt`** : export JSON `excalidraw/clipboard`, 1179 éléments dont 32 frames. Frames étudiées : « Node model », « DAG », « Config », « ProcessContext », « ChannelID », « Connection », « BufferPool », « ExecutionTask », « ExecutionPlan », « Compile DAG », « DoubleBuffer », « ExchangeState », « pipeline » et schémas de séquences de compilation/publication.
4. **`mka-audio-specs(1).excalidraw`** et autres maquettes du projet : contexte des modèles visuels et des versions antérieures. La documentation présente ci-jointe intègre des diagrammes Mermaid **recréés et éditables**, et non un export bitmap fidèle pixel à pixel de l'Excalidraw.

## Contradictions historiques réconciliées explicitement

| Ancienne information | État actuel / source de priorité |
|---|---|
| `DAG::connect` refuse canal absent du layout (`reject connexion on undeclared ports`) | **Remplacée** par décision explicite d'autoriser les connexions structurellement valides même si le canal n'est pas traité. |
| `nodeID:u64` dans schéma Node | `ChannelID` validé sur 64 bits réserve **32 bits** pour NodeID. Les types sources doivent être harmonisés. |
| `frames = pool.maxFrames` dans une task compilée | La **capacité** est `maxFrames`, mais `context.frames` RT doit valoir **les frames effectifs** du callback. |
| `outputsNodes` signale seulement des buffers internes non recyclables | **Évolué** : les sorties marquées écrivent librement directement dans les buffers backend. Les buffers internes nécessaires en amont restent soumis au liveness. |
| `inputsNodes` ajoutée comme liste symétrique | **Abandonnée** : source = nœud sans connexion entrante, classé par la topologie compilée. |
| `InputBinding`/copie matérielle ou `NodeID` hardware spécial | **Abandonné** : branchement direct de vues des entrées/sorties dans les contextes. |
| Une sortie matérielle doit avoir un écrivain exclusif | **Refusée** : LWW, pas de validation de propriété par canal. |
| PDC et latence « après V1 » du journal 25/10 | **Remplacé** : PDC déclaré dernière fonction de V1, analyse et DelayTask discutées fin octobre. |
| Exception dans resize pool dessinée sur la maquette | Contrat public privilégie `std::expected`, pas d'exception laissée dans callback ; les erreurs d'allocation sont traitées hors RT. |
| « double buffer de samples » pour PDC | Précisé : historique **circulaire** sample-exact ; le double buffer A/B sert à la publication des plans. |

## Hypothèses de présentation

Les fiches contiennent des champs privés/propositions de classes pour rendre le système implémentable et explicable. Ils ne sont pas garantis comme déjà présents dans le code source. Les noms exacts des méthodes `Result`, `CompileConfig`, `InputChannel`, `OutputChannel`, `DelayTask`, `TaskIndex` restent à harmoniser avec le dépôt lors de l'implémentation. Le journal distingue systématiquement le **contrat validé** de la **forme recommandée**.

## Traçabilité

Toutes les décisions `JD-001..JD-039` ont été reformulées et reliées aux fiches modules, aux algorithmes et aux tests. Les diagrammes Mermaid préservent la structure de recherche sans reprendre les parties obsolètes de l'Excalidraw comme exigences actives.
