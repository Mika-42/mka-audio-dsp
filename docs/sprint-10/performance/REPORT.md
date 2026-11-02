# Mesures V1 — observations locales

120 scénarios, 100 mesures après 20 callbacks de chauffe CPU, soit 12 000 durées brutes. Fréquence 48 kHz, C++26 Release Clang 19/libc++19, -O3 -DNDEBUG. Voir environment.json pour CPU/OS, quota de conteneur, flags exacts, charge et hash du harness.

| frames | budget µs | pire médiane µs | pire p95 µs | pire p99 µs | max observé µs | dépassements / 2400 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 20.833 | 2.390 | 2.533 | 2.909 | 29.209 | 2 |
| 64 | 1333.333 | 32.480 | 127.464 | 400.130 | 872.707 | 0 |
| 128 | 2666.667 | 63.459 | 238.674 | 471.250 | 1264.683 | 0 |
| 512 | 10666.667 | 290.851 | 731.800 | 1445.844 | 1988.319 | 0 |
| 4096 | 85333.333 | 3026.956 | 5691.790 | 7639.801 | 8981.945 | 0 |

Chaque percentile de ce tableau est le maximum des percentiles de scénario, pas un percentile regroupé. Le résumé CSV donne aussi moyenne, Nodes/tâches, slots, RAM PCM/silence/historiques/plan, latence cumulée max, contrôles de capacité, publications et refus mémoire. `branches` compte les lecteurs/jonctions parallèles du fan-out ; chacune joint un chemin rapide et un lent. Les fan-out 8/64 utilisent deux sources. Tous les scénarios existent sans/avec PDC et avec 2/6 sorties. Le cas publish inclut 12 publications, dont deux dans la chauffe.

Les probes validés par contrôles positifs comptent **0 allocation/libération C++ ou C directe, 0 mutex, attente et log directs** dans les 14 400 fenêtres exec (chauffe incluse). Chaque scénario refuse aussi un candidat au budget nul, hors timing. Le compteur de capacité est nul. La RAM est le payload retenu du plan ; elle exclut les Nodes DSP, le bookkeeping allocateur et les temporaires de compilation. Deux générations coexistent pendant une publication : ne pas confondre ce total par plan avec la RAM maximale de tout le processus.

La chauffe CPU ne garantit pas D samples par génération : les publications conservent le warmup PDC nul autorisé.

Les horloges et CSV sont dans le harness, hors exec. La mesure inclut l'appel, ses bornes de probe et l'adoption de B, sans la compilation/retraite contrôle. Conteneur partagé, pas de pinning ni priorité RT, boucle non cadencée : préemption, fréquence CPU et cache perturbent les maxima. Les dépassements sont conservés dans les données ; aucun chiffre ne garantit une deadline sur un autre système. Quantiles au rang ceil(q*N)-1, résolution du steady_clock et 100 observations par scénario ; pas d'estimation statistique de pannes rares. Les wrappers GNU observent les appels directement liés depuis le harness/moteur, pas tous les appels internes d'un plugin/binaire tiers.

## Reproduction

```sh
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DMKA_BUILD_TESTS=OFF -DMKA_BUILD_BENCHMARKS=ON -DCMAKE_CXX_COMPILER=clang++-19
cmake --build build/bench --parallel 2
python3 tools/run-benchmark.py --build build/bench --output build/performance
```

Utiliser une STL C++26 compatible (libc++19 ou libstdc++16) et ses flags/toolchain si elle n'est pas celle par défaut. Le harness Linux refuse TSan dans ce même binaire ; les sanitizers ont leurs builds distincts. `tools/validate-benchmark.py` recalcule les statistiques depuis raw.csv et vérifie budgets, mémoire et compteurs. Les observations commitées sont conservées dans ce dossier, sans sélection des valeurs favorables.
