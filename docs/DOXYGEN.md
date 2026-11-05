# Generating the C++ reference

Author: Mika

Run Doxygen 1.9.8 or later from the repository root:

```sh
doxygen Doxyfile
```

Open `build/docs/html/index.html` to browse the HTML reference. The same run
writes XML to `build/docs/xml` and documentation diagnostics to
`build/docs/warnings.log`. Generated files stay outside version control.

The existing CMake documentation target is another entry point:

```sh
cmake --preset debug -DMKA_BUILD_DOCS=ON
cmake --build build/debug --target doc
```

The reference includes the engine in `src`, the short usage examples in
`exemple`, the benchmark harness in `bench`, and the tests and fixtures in
`test`. Private helpers and anonymous namespaces are included so that resource
ownership, compilation steps and validation support can be read alongside the
public API. The file pages provide annotated source links.

GoogleTest declarations are expanded into named documentation entries only
while Doxygen runs. This affects documentation parsing, not compilation or
test behavior. Undocumented declarations and malformed documentation produce
warnings; warnings fail the documentation build.

Comments describe each declaration's purpose. Ownership, thread requirements,
preconditions and failure behavior are included where they affect correct use.
The [API contract](API_V1.md) collects the shared lifetime and callback rules.
