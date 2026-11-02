# Recette finale V1 — sprint 10

Toutes les fiches S10-01 à S10-07 sont couvertes par le contenu de cette série. La validation porte sur le contenu de cette PR, dont le parent et l'empreinte des sources figurent dans [environment.json](environment.json). Les [résultats structurés](results.json) donnent les compilateurs, flags, options runtime, compteurs et durées exactes. Aucune suppression sanitizer, aucun test sauté et aucun filtre de tests dans ces quatre suites complètes.

| Profil C++26 | Réussis / total | Dont probes RT | Durée observée | Journaux |
|---|---:|---:|---:|---|
| Clang/libc++ 19.1.7 Debug | 398/398 | 44 | 54.73 s | [CTest](clang-ctest.log), [JUnit gzip](clang-junit.xml.gz), [stress](clang-stress.log) |
| GCC/libstdc++ 16.2 Debug | 398/398 | 44 | 35.97 s | [CTest](gcc-ctest.log), [JUnit gzip](gcc-junit.xml.gz), [stress](gcc-stress.log) |
| Clang 19 ASan + UBSan + LeakSanitizer | 398/398 | 44 | 174.66 s | [CTest](asan-ctest.log), [JUnit gzip](asan-junit.xml.gz), [stress](asan-stress.log) |
| Clang 19 TSan | 354/354 | 0 | 45.65 s | [CTest](tsan-ctest.log), [JUnit gzip](tsan-junit.xml.gz), [stress](tsan-stress.log) |

TSan exécute toute la suite principale de 354 tests. Les 44 tests avec interposition globale new/delete sont isolés dans le binaire RT et exécutés en natif et ASan ; ils sont exclus du profil TSan pour éviter une interposition incompatible. ASan conserve la détection des fuites. Les journaux de build correspondants sont `clang-build.log`, `gcc-build.log`, `asan-build.log` et `tsan-build.log` dans ce dossier. Les durées incluent la contention du conteneur et ne sont pas des mesures de deadline audio.

La recette `V1Acceptance` utilise deux entrées physiques const, Gain=0.5, une branche directe compensée et une branche DSP retardée de D, puis somme In+Aux. L'oracle attendu est x[n−D] (zéro avant D), identique sur casque/enceintes ; le dernier writer double les samples pairs de la voie 0 seulement. Pour D=0/64/256/512, les voies 4/5 restent nulles, la sortie logique demeure indépendante du hardware, les pointeurs physiques alternent et chaque sample/sentinelle est vérifié. Les blocs 0/1/2/7/128 ne réinitialisent pas l'histoire.

Chaque profil rejoue également **10 000 cycles / 20 000 callbacks**, graine **0x10CAFE42**, génération finale **10001**, **12006 Nodes créés et détruits**. A rend pendant la compilation de B ; Pending reste stable sans callback, puis adoption frontière/ACK/retrait. C peut être édité pendant Pending. Les erreurs de budget/overflow/allocation laissent la génération audible intacte ; les Nodes retirés restent empruntés jusqu'à ACK. Aucun destructeur de Node n'est exécuté dans la fenêtre audio.

## Risques et preuves

| Risque | Preuve exécutée | Portée |
|---|---|---|
| UAF, dépassement, fuite, UB | 398 tests ASan/UBSan/LeakSanitizer, sentinelles et refus mémoire | Moteur et fake DSP du dépôt |
| Race publication/retrait/édition/diagnostic | 354 tests TSan, stress deux threads et drain atomique | Un thread audio et un thread contrôle, stop/join respecté |
| Mauvais PDC, warmup ou déclaration de latence | Oracles d'impulsions/rampes, D=0/3/64/128/256/512, P-01..P-12 | Latences déclarées et jonctions logiques connues |
| Mauvais routage, LWW ou pointeur persistant | Recette intégrée, tests IO 2/6 voies, banques alternées et successeur logique | Pas d'arête hardware, writes directs des Nodes marqués |
| Traitement RT interdit | 44 tests RT ; recette avec zéro new/delete/mutex/attente/log/exception directs | Fenêtres strictement autour de exec, compteurs TLS sans allocation |
| Compteur inopérant | Contrôles positifs heap, mutex, nanosleep, write et throw | Contrôles volontaires exécutés hors moteur ; GNU --wrap Linux |
| Temps ou RAM excessifs | [120 scénarios / 12000 durées brutes](../performance/REPORT.md), budget nul refusé et mémoire recomptée | 2 dépassements observés conservés, sans garantie de deadline universelle |
| Documentation obsolète ou test introuvable | [51 exigences validées](traceability.log), HTML/XML Doxygen, zéro warning | Symboles et noms CTest réels ; types centraux vérifiés dans XML |

Les compteurs RT de la recette observent les appels C++/pthread/nanosleep/write/fwrite/__cxa_throw directement liés au binaire. Le benchmark ajoute les probes malloc/free/calloc/realloc directs. Ils ne détectent pas tous les appels internes d'une bibliothèque tierce ou des syscalls arbitraires. Le contrat noexcept fait aussi échouer le processus si une exception s'échappe du callback. Aucun test sur carte son, driver réel ni plugin binaire externe n'est revendiqué ; l'alignement physique inconnu, les tails, MIDI, parallélisme DSP, migration/crossfade restent hors V1. Une mesure locale verte ne prouve pas l'absence de toutes les races possibles ni une garantie de scheduling RT.

## Reproduction

CMake ≥4.3, Ninja, Python 3, Doxygen et un compilateur/STL C++26 compatibles sont nécessaires. Choisir Clang 19 avec libc++19 ou GCC/libstdc++16, en passant les flags ou le toolchain de l'installation locale lors de la première configuration. Le cloud utilisait un sysroot local ; les flags effectifs sont conservés dans results.json.

```sh
cmake --preset debug -DCMAKE_CXX_COMPILER=clang++-19 -DMKA_BUILD_DOCS=ON
cmake --build --preset debug --parallel 2
ctest --preset debug --output-junit "$PWD/build/debug/results.xml"
cmake --build build/debug --target doc
python3 tools/check-requirements.py --build build/debug
python3 tools/validate-benchmark.py docs/sprint-10/performance
# Deux builds distincts ; ne pas désactiver leaks ni les diagnostics sanitizer.
CXX=clang++-19 tools/check-sanitizers.sh
# Pour lire un JUnit archivé : gzip -dc docs/sprint-10/validation/asan-junit.xml.gz
```

Pour le second compilateur, configurer un autre dossier Debug avec `-DCMAKE_CXX_COMPILER=g++-16`, puis build et ctest sans filtre. Le script sanitizer enregistre le compilateur réellement choisi par CMake, y compris en présence d'un toolchain. [doxygen.log](doxygen.log) et [benchmark-validation.log](benchmark-validation.log) complètent les journaux ; le répertoire HTML/XML généré est régénérable et ignoré par Git.
