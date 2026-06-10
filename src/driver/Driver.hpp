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

    // §2.4 / §2.6 (issue #328 inc 3): compile `path` to a native OBJECT at
    // `objectPath` (kept, not linked), with its public-interface metadata
    // embedded in the `.bezel.meta` section (§2.6 step 4). This is the
    // dependency-producing path: a module compiled this way is then passed as a
    // Dependency to an importer's compileFile. Unlike the executable path, no
    // `Main` is required (§33.6: a library has no entry point) and no link runs.
    // `deps` lets a dependency itself import further dependencies (acyclic).
    CompileResult compileObject(const std::string& path,
                                const std::string& objectPath,
                                const std::vector<Dependency>& deps = {});
};

} // namespace bezel
