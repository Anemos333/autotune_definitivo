# Integrazione rapida

1. Creare un branch:

```bash
git switch -c feature/wind-fix-v6
```

2. Copiare `WindFixV6_git.patch` nella root del repository e applicarlo:

```bash
git apply --check WindFixV6_git.patch
git apply WindFixV6_git.patch
```

3. Aggiungere i test al CMake principale:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/Tests/AutotuneTests.cmake)
```

4. Configurare e compilare:

```bash
cmake -S . -B build-v6 -DBUILD_TESTING=ON -DAUTOTUNE_BUILD_TESTS=ON
cmake --build build-v6 --config Release --parallel
ctest --test-dir build-v6 -C Release --output-on-failure
```

5. Prima del merge, creare render A/B nelle tre modalità e controllare il pannello diagnostico `Poly / Rel / Mask / Hold`.
