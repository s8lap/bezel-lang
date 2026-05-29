#pragma once

// Compilation driver: orchestrates the phases for one invocation of the
// compiler. The pipeline order — lex -> parse -> type-check -> ownership-check
// -> codegen — reflects §2 (Modules and Compilation) and §33 (Program Entry
// Point). Each phase is currently a stub, so the driver wires them together
// but performs no real compilation.

#include <cstddef>
#include <string>

namespace bezel {

struct CompileResult {
    bool success = false;
    std::size_t errorCount = 0;
};

class Driver {
public:
    // Compiles a single source file identified by path. The file is read, then
    // handed through the phase pipeline. Diagnostics are printed to stderr.
    // When `outputPath` is non-empty and the front-end produces no errors, a
    // native executable is emitted there (object via LLVM + link via clang).
    CompileResult compileFile(const std::string& path,
                              const std::string& outputPath = "");
};

} // namespace bezel
