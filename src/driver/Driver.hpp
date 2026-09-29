#pragma once

// Compilation driver: orchestrates the phases for one invocation of the
// compiler. The pipeline order — lex -> parse -> type-check -> ownership-check
// -> codegen — reflects §2 (Modules and Compilation) and §33 (Program Entry
// Point). Each phase is currently a stub, so the driver wires them together
// but performs no real compilation.

#include <cstddef>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"

namespace bezel {

struct CompileResult {
    bool success = false;
    std::size_t errorCount = 0;
};

// §2.5 / §2.6 (issue #328 inc 3): a dependency artifact passed to the importer's
// compilation. `metadataPath` is the artifact whose embedded `.bezel.meta`
// section the loader reads (the dependency's object or executable, §2.6 step 4);
// `objectPath`, if set, is the dependency's native object to link into the
// importer's executable so cross-module calls resolve at link time. When the
// metadata and object live in the same artifact, both fields hold that path.
struct Dependency {
    std::string metadataPath;
    std::string objectPath;
};

class Driver {
public:
    // Compiles a single source file identified by path. The file is read, then
    // handed through the phase pipeline. Diagnostics are printed to stderr.
    // When `outputPath` is non-empty and the front-end produces no errors, a
    // native executable is emitted there (object via LLVM + link via clang).
    CompileResult compileFile(const std::string& path,
                              const std::string& outputPath = "");

    // §2.5 / §2.6 (issue #328 inc 3): compile `path` as an importer against the
    // given dependency artifacts. Each dependency's `.bezel.meta` is loaded into
    // a cross-module symbol table BEFORE sema runs (so `import`s and cross-module
    // references resolve and type-check, §2.6 steps 2-3); each dependency's
    // object is linked into the final executable so cross-module calls resolve.
    // With an empty `deps`, behaves exactly like compileFile (the import path is
    // additive — single-module compilation is unaffected).
    CompileResult compileFile(const std::string& path,
                              const std::string& outputPath,
                              const std::vector<Dependency>& deps);

    // Overload with explicit codegen options (e.g., --checked-disposal / §28.2).
    CompileResult compileFile(const std::string& path,
                              const std::string& outputPath,
                              const std::vector<Dependency>& deps,
                              const codegen::CodegenOptions& opts);

    // ---- the two halves of the executable build ---------------------------
    // compileFile(path, out, deps, opts) IS
    //     compileExecutableObject(path, out + ".o", deps, opts)
    //   then linkExecutableObject(out + ".o", out, deps, opts),
    // with the object removed afterwards. Exposing the halves lets a caller that
    // caches build products key each half on its OWN inputs (see
    // tests/support/aot_support.hpp): the emitted object depends on this
    // compiler + `opts` + the source text, while the link additionally depends
    // on the runtime archive and nothing else. Because compileFile is literally
    // their composition, the split path and the shipping path cannot drift.

    // The FRONT-END half: run the full executable-build front-end (§2.6, plus
    // the §33.5 entry-point check) and emit the native object at `objectPath`,
    // performing NO link. An empty `objectPath` runs the front-end only, with no
    // codegen and no entry-point check — the `compileFile(path, "")` shape.
    //
    // §2.8.1 rules 3+5 (#1729, v0.188): when `survivingDepPaths` is non-null,
    // it receives the subset of `deps` whose artifacts the loader actually
    // kept (one per moduleName, after the rule-3 dedup). `compileFile` uses
    // it to filter the dependency list passed to the link step so a
    // diamond-assembly duplicate is not double-linked.
    CompileResult compileExecutableObject(const std::string& path,
                                          const std::string& objectPath,
                                          const std::vector<Dependency>& deps,
                                          const codegen::CodegenOptions& opts,
                                          std::vector<std::string>* survivingDepPaths = nullptr);

    // The LINK half: link `objectPath` (plus each dependency's object) into the
    // executable `exePath` with the project's clang recipe. Prints
    // "bezelc: link error" and returns false on failure.
    bool linkExecutableObject(const std::string& objectPath,
                              const std::string& exePath,
                              const std::vector<Dependency>& deps,
                              const codegen::CodegenOptions& opts);

    // The single definition of the link recipe (see Driver.cpp). `runtimeLibPath`
    // is appended verbatim when non-empty; pass runtimeLibraryPath() for the real
    // link, or a placeholder to obtain a path-independent fingerprint.
    static std::string linkCommandLine(const std::string& objectPath,
                                       const std::string& exePath,
                                       const std::vector<std::string>& extraObjects,
                                       bool sanitizeAddress,
                                       const std::string& runtimeLibPath);

    // The on-disk Bezel runtime archive the link consumes ("" if CMake injected
    // no path). This is the ONLY runtime-derived input to a Bezel build: codegen
    // neither includes a runtime header nor reads the archive.
    static std::string runtimeLibraryPath();

    // Every auto-imported stdlib prelude source, concatenated (NUL-separated) —
    // the text injected into every compilation unit. A codegen INPUT, so a cache
    // over emitted objects must key on it; see Driver.cpp for why this is
    // exposed rather than left to the compiler archive's identity to cover.
    static std::string preludeSourceFingerprint();

    // linkCommandLine with its per-invocation paths replaced by placeholders —
    // a stable fingerprint of the recipe for cache keying. Every flag, library,
    // and their order appear verbatim; only the object/executable/runtime PATHS
    // are abstracted (a cache keys on the runtime's content instead).
    static std::string linkRecipeSignature(bool sanitizeAddress);

    // §2.4 / §2.6 (issue #328 inc 3): compile `path` to a native OBJECT at
    // `objectPath` (kept, not linked), with its public-interface metadata
    // embedded in the `.bezel.meta` section (§2.6 step 4). This is the
    // dependency-producing path: a module compiled this way is then passed as a
    // Dependency to an importer's compileFile. Unlike the executable path, no
    // `Main` is required (§33.6: a library has no entry point) and no link runs.
    // `deps` lets a dependency itself import further dependencies (acyclic).
    //
    // `opts` is the build's codegen options and this path DERIVES the library
    // build's from it (fix #4488), overriding exactly two fields:
    //   * `isMainModule` is forced false — §33.6, a library has no entry point;
    //   * `sanitizeAddress` is forced false — it is an executable-build option
    //     that needs the final link to add the ASan runtime, and this path does
    //     not link (main.cpp rejects `-c --sanitize=address`). Leak/double-free
    //     coverage of the dependency survives via the executable's runtime.
    // Everything else is carried through, because both checked modes are
    // per-build choices (§28.2, §22.16) whose instrumentation is *emitted*:
    // a dependency built without it has no checks and no link step can add
    // them. Deriving rather than default-constructing is deliberate — a field
    // added to CodegenOptions later is forwarded here by default.
    CompileResult compileObject(const std::string& path,
                                const std::string& objectPath,
                                const std::vector<Dependency>& deps = {},
                                const codegen::CodegenOptions& opts = {});
};

} // namespace bezel
