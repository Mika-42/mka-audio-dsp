# mka-audio-dsp

Moteur de graphes audio DSP en **C++26**, organisé en modules C++. Il permet de
relier des processeurs dans un graphe orienté sans cycle, de préparer leur
exécution hors du thread audio et de remplacer le plan actif pendant la lecture.

- **Nodes personnalisés** : entrées, sorties et auxiliaires logiques, canaux mono
  ou multicanaux, traitement `process() noexcept`.
- **Exécution préparée** : ordre topologique stable, buffers réutilisés selon la
  durée de vie des signaux, contrôles de capacité et budget mémoire.
- **Compensation de latence (PDC)** : latences déclarées par les Nodes, retards
  calculés aux jonctions et historiques persistants entre blocs.
- **Routage matériel** : entrées physiques en lecture seule, sorties accessibles
  aux Nodes marqués output, voies non écrites silencieuses et Last Writer Wins.
- **Modification en lecture** : deux plans A/B, publication à une frontière de
  callback, acquittement et destruction différée sur le thread de contrôle.

## Compiler et essayer

Prérequis : **CMake ≥ 4.3**, Ninja et un compilateur/STL compatible C++26 et
modules, par exemple **GCC/libstdc++ 16** ou **Clang 19 avec libc++ 19**.
GoogleTest 1.17 est téléchargé par CMake à la première configuration.

```sh
cmake --preset debug -DCMAKE_CXX_COMPILER=g++-16 -DMKA_BUILD_EXAMPLES=ON
cmake --build --preset debug --parallel 2
./build/debug/mka-graph-example
ctest --preset debug
```

Pour Clang ou une installation particulière, fournir le compilateur et les flags
de la STL, ou un fichier toolchain, lors de la première configuration.

## Exemple et documentation

[exemple/graph_exemple.cpp](exemple/graph_exemple.cpp) décrit un petit mixeur
stéréo avec gain, branches directe/latente, Mix In/Aux et sorties casque/enceintes.
Il exécute et vérifie le rendu, remplace une latence de 64 par 256 samples,
illustre les refus, le swap A/B, le retrait des Nodes et l'arrêt/reprise.
Le backend est simulé ; aucune carte son n'est nécessaire.

Les allocations, éditions du graphe, préparations et diagnostics formatés restent
sur le contrôle. Le callback traite des buffers empruntés sans allocation,
libération, attente, verrou, log ni exception. Les nouveaux historiques de
compensation commencent à zéro : une transition sans clic n'est pas garantie.

- [API et contrats de propriété](docs/API_V1.md)
- [Validation V1, sanitizers et journaux](docs/sprint-10/validation/REPORT.md)
- [Mesures CPU et mémoire](docs/sprint-10/performance/REPORT.md)

Pour générer la documentation : configurer avec `-DMKA_BUILD_DOCS=ON`, puis
`cmake --build build/debug --target doc` (Doxygen requis ; HTML dans
`build/docs/html`).
