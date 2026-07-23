#include "driver/Driver.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <cstdlib>

#include "ast/Node.hpp"
#include "codegen/CodeGen.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "ownership/Ownership.hpp"
#include "ownership/OwnershipChecker.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/EntryPoint.hpp"
#include "sema/PureChecker.hpp"
#include "sema/StaticInitCycles.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeChecker.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace bezel {

namespace {

// Reads an entire file into `out`. Returns false if the file cannot be opened.
bool readFile(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

// stdlib §2.20 (v0.200, #2047): the auto-imported `Result<T, E>` choice. The
// stdlib ships exactly this one choice (per §35); it is a pure Bezel choice with
// no compiler intrinsic, so it is made available by SYNTHESIZING its declaration
// from the verbatim stdlib §2.20 source and injecting it as a top-level decl into
// every compilation unit. The full choice machinery — §35.4 per-instantiation
// classification, construction, match, §35.4 disposal, §35.7 equality/hash —
// then applies to it exactly as to a user-declared choice. Choices are not
// embedded in module metadata, so the injected `public` decl never collides
// across a module import boundary.
const char kResultPreludeSource[] =
    "module __bezel_prelude_result__;\n"
    "public choice Result<T, E>\n"
    "  Ok(owned value: T),\n"
    "  Error(owned error: E);\n"
    "\n"
    "  public method IsOk(): bool => (case this of\n"
    "    Ok(_)    => true,\n"
    "    Error(_) => false);\n"
    "\n"
    "  public method IsError(): bool => not IsOk();\n"
    "end Result;\n";

// Parse the auto-imported stdlib prelude and PREPEND its top-level declarations
// to `unit`. The prelude source is a program-lifetime constant, so the AST
// string_views into it stay valid for the whole compilation; ownership of the
// parsed nodes transfers into `unit->topDecls`. A private diagnostics sink
// absorbs any prelude parse diagnostic (the prelude is known-valid — a
// diagnostic here would be an internal compiler defect, never the user's fault).
void injectStdlibPrelude(ast::SourceFile& unit) {
    static const std::string preludeSrc(kResultPreludeSource);
    DiagnosticEngine preludeDiags;
    Lexer preludeLexer(preludeSrc, preludeDiags);
    Parser preludeParser(preludeLexer, preludeDiags);
    std::unique_ptr<ast::SourceFile> preludeUnit =
        preludeParser.parseCompilationUnit();
    if (!preludeUnit || preludeDiags.hasErrors()) return;  // defensive: no-op
    // #2047 follow-up 2: an auto-imported prelude type is SHADOWED by a user
    // top-level declaration of the same name (standard auto-import shadowing) —
    // inject a prelude decl ONLY when the user declares no type of that name.
    // Without this, a program that declares its own `Result` (the corpus does —
    // e.g. a `sealed abstract class Result<T>` hierarchy) collides with the
    // injected `Result<T, E>` choice: two same-named top-level types corrupt name
    // resolution and per-monomorphization descriptor emission for the WHOLE
    // module (an unrelated `is Box<string>` then mis-resolves). The user's type
    // wins; the stdlib `Result` is simply unavailable in that module.
    std::unordered_set<std::string_view> userTypeNames;
    for (const auto& td : unit.topDecls)
        if (auto* decl = dynamic_cast<const ast::TypeDecl*>(td.get()))
            userTypeNames.insert(decl->name);
    std::vector<std::unique_ptr<ast::TopDecl>> toInject;
    for (auto& td : preludeUnit->topDecls) {
        auto* decl = dynamic_cast<ast::TypeDecl*>(td.get());
        if (!decl) continue;
        if (userTypeNames.count(decl->name)) continue;  // user shadows the prelude
        // Mark each injected type decl as synthesized so codegen emits its
        // functions with INTERNAL linkage — every module gets a private copy, so
        // external linkage would collide when two modules link.
        decl->isSynthesizedPrelude = true;
        toInject.push_back(std::move(td));
    }
    // Prepend so the choice precedes any user decl (declaration order is not
    // significant for type resolution, but this keeps a user forward-reference
    // to `Result` unambiguous). Move each surviving TopDecl's ownership into
    // `unit`.
    unit.topDecls.insert(
        unit.topDecls.begin(),
        std::make_move_iterator(toInject.begin()),
        std::make_move_iterator(toInject.end()));
}

// Link a native object (plus any §2.5 dependency objects) into an executable
// using clang (the project's toolchain). Returns true on success. The
// dependency objects supply the definitions of the imported symbols the
// importer's object references as externals (issue #328 inc 3).
bool linkExecutable(const std::string& objectPath,
                    const std::string& exePath,
                    const std::vector<std::string>& extraObjects = {},
                    bool sanitizeAddress = false) {
    // Link with libc, libm, libpthread, and the Bezel runtime library.
    std::string cmd = "clang " + objectPath;
    for (const std::string& obj : extraObjects) cmd += " " + obj;
    cmd += " -o " + exePath + " -lm -lpthread";
    if (sanitizeAddress) {
        // JIT→AOT test-harness migration §2 (memory net): link the ASan runtime
        // (defines __asan_report_* / asan.module_ctor referenced by the
        // AddressSanitizer-instrumented object) and keep debug info so reports
        // symbolise to source locations. Only added when the object was emitted
        // with --sanitize=address; the default link line is unchanged.
        cmd += " -fsanitize=address -g";
    }
#ifdef BEZEL_RUNTIME_LIB_PATH
    // Static-link the Bezel runtime (path injected at compile time by CMake).
    cmd += " ";
    cmd += BEZEL_RUNTIME_LIB_PATH;
#endif
    cmd += " -lstdc++";  // runtime uses C++ std (std::mutex, std::thread, etc.)
    return std::system(cmd.c_str()) == 0;
}

} // namespace

CompileResult Driver::compileFile(const std::string& path,
                                  const std::string& outputPath) {
    // The no-dependency call is single-module compilation, unchanged.
    return compileFile(path, outputPath, /*deps=*/{});
}

CompileResult Driver::compileFile(const std::string& path,
                                  const std::string& outputPath,
                                  const std::vector<Dependency>& deps) {
    return compileFile(path, outputPath, deps, codegen::CodegenOptions{});
}

CompileResult Driver::compileFile(const std::string& path,
                                  const std::string& outputPath,
                                  const std::vector<Dependency>& deps,
                                  const codegen::CodegenOptions& opts) {
    std::string source;
    if (!readFile(path, source)) {
        std::cerr << "bezelc: error: cannot open '" << path << "'\n";
        return CompileResult{false, 1};
    }

    DiagnosticEngine diagnostics;

    // Phase pipeline (§2, §33). Each phase is a stub today; this establishes
    // the order in which the milestones will fill them in.
    Lexer lexer(source, diagnostics);
    Parser parser(lexer, diagnostics);
    auto unit = parser.parseCompilationUnit();

    // §2.6 (build phases): parse must complete cleanly before the later
    // phases (type resolution, type-check, ownership) run. The parser returns
    // an error-recovery AST even after queueing parse diagnostics, and the
    // front-end passes unconditionally deref AST children that the grammar
    // guarantees non-null but a partial recovery leaves null (e.g. a `const`
    // whose initializer is the malformed `1 + ;` leaves a `BinaryExpr` with a
    // null right operand). Running those passes on such a tree crashed the
    // compiler — a `unique_ptr::operator*()` abort — before the already-queued
    // parse diagnostic was ever printed (#1622; #1615 was one such deref).
    // Gate the whole front-end on a clean parse, exactly as the entry-point
    // check and codegen below already gate on a clean front-end. Valid programs
    // (no parse errors) are unaffected: the block runs precisely as before.
    if (unit && !diagnostics.hasErrors()) {
        // stdlib §2.20 (#2047): make the auto-imported `Result<T, E>` choice
        // available by injecting its synthesized declaration before symbol
        // collection, so user references to `Result<T, E>` resolve.
        injectStdlibPrelude(*unit);
        // Sema milestone — runs in pass order: symbol collection,
        // type resolution, expression typecheck. Each pass annotates
        // the AST via side-tables on SemaContext.
        sema::SemaContext semaCtx(diagnostics);
        sema::collectSymbols(*unit, semaCtx);
        // §2.5 / §2.6 step 2 (issue #328 inc 3): load each dependency's
        // public-interface metadata into the cross-module symbol table BEFORE
        // type resolution, so the importer's `import`s and cross-module
        // references bind (§2.2) and type-check (§2.6 step 3) against the
        // imported signatures. Symbol collection runs first (it establishes
        // THIS module's symbols); the loader adds imported types as a separate,
        // import-gated set. With no dependencies this loop is a no-op and the
        // pipeline is byte-for-byte the single-module path.
        // §2.5 / #1487: load ALL dependencies in two global passes (all exported
        // type shells registered, then all members filled) so a Named reference
        // in one dependency to a user type declared in ANOTHER binds by name
        // regardless of the order the `--import`s are listed. The old per-dep loop
        // resolved artifact N's references before artifact N+1 loaded, silently
        // degrading a forward reference to a garbage-valued TypeRef.
        {
            std::vector<std::string> depMetaPaths;
            depMetaPaths.reserve(deps.size());
            for (const Dependency& dep : deps)
                if (!dep.metadataPath.empty())
                    depMetaPaths.push_back(dep.metadataPath);
            // §2.8.4 rule 4 (#1733): loadDependencies sets `depErr` when it
            // rejects an executable artifact presented as a dependency; print that
            // (it names the artifact + its kind) instead of the generic message.
            std::string depErr, failedPath;
            if (!depMetaPaths.empty() &&
                !bezel::module::loadDependencies(depMetaPaths, semaCtx, &depErr,
                                                 &failedPath)) {
                if (!depErr.empty())
                    std::cerr << "bezelc: error: " << depErr << "\n";
                else
                    std::cerr
                        << "bezelc: error: cannot load module metadata from '"
                        << failedPath << "'\n";
                return CompileResult{false, 1};
            }
        }
        sema::resolveTypes(*unit, semaCtx);
        sema::typecheckExpressions(*unit, semaCtx);
        // §22.15 (v0.202, #2052): enforce the pure-body rule set on every
        // pure member. Runs after typecheck so resolved call targets and
        // expression types are available.
        sema::checkPureBodies(*unit, semaCtx);
        // Definite-assignment runs after expression typecheck so it
        // can see the same scope structure ExprTypecheck built.
        sema::checkDefiniteAssignment(*unit, semaCtx);
        // §2.5 / §22.13 (#797): reject circular static-initialization
        // dependencies — runs after typecheck so static-member references are
        // resolved.
        sema::checkStaticInitCycles(*unit, semaCtx);

        // Ownership milestone — runs after sema since it needs
        // resolved local symbols + producer info.
        ownership::checkOwnership(*unit, semaCtx);

        // Stub passes kept around so the milestone scaffolding doesn't
        // bit-rot — they no-op until their own sub-milestones land.
        TypeChecker typeChecker(diagnostics);
        typeChecker.check(*unit);

        OwnershipChecker ownershipChecker(diagnostics);
        ownershipChecker.check(*unit);

        // §33.5: an executable build requires exactly one qualifying `Main`.
        // Scoped to the executable path (non-empty outputPath) so library
        // compilation and the sema/JIT test harnesses — which run the shared
        // front-end on snippets that legitimately omit `Main` (§33.6) — are
        // not failed by this requirement. Only run when the front-end is
        // otherwise clean, to avoid cascading on already-broken programs.
        if (!outputPath.empty() && !diagnostics.hasErrors()) {
            sema::checkEntryPoint(*unit, semaCtx);
        }

        // Codegen: when an output path is requested and the front-end is
        // clean, lower to LLVM, emit a native object, and link an
        // executable with clang.
        if (!outputPath.empty() && !diagnostics.hasErrors()) {
            llvm::LLVMContext llvmCtx;
            auto module = codegen::emitModule(*unit, semaCtx, llvmCtx, opts);
            // §2.6 step 4: embed the module's public-interface metadata in a
            // dedicated `.bezel.meta` object section so downstream `import`
            // resolution (§2.5) can read this module's interface without its
            // sources. Single call into the builder/emit helper (#328 inc 2).
            if (module) {
                // §14 / #662c: pass `source` so ctor/dtor bodies for generic
                // classes are span-extracted into the metadata (Foundation C).
                // §2.8.4 rule 1 (#1733): this is the EXECUTABLE build path
                // (non-empty outputPath, entry-point-checked above) — record
                // the artifact as executable-kind so an importer rejects it as a
                // dependency (§2.8.4 rule 4).
                bezel::module::embedModuleMetadata(*module, semaCtx, source,
                                                   /*isExecutable=*/true);
            }
            std::string objectPath = outputPath + ".o";
            std::string err;
            if (!module || !codegen::emitObjectFile(*module, objectPath, err,
                                                    opts.sanitizeAddress)) {
                // Print any diagnostics recorded during codegen (e.g. "not yet
                // implemented") before reporting the codegen failure (#628 pt 1).
                for (const Diagnostic& diag : diagnostics.diagnostics()) {
                    std::cerr << path << ":" << diag.location.line << ":"
                              << diag.location.column << ": " << diag.message
                              << "\n";
                }
                if (!err.empty())
                    std::cerr << "bezelc: codegen error: " << err << "\n";
                return CompileResult{false, diagnostics.errorCount() + 1};
            }
            // §2.6 step 4: link the importer's object together with each
            // dependency's object, so the external references the importer's
            // object holds (issue #328 inc 3) resolve against the dependency's
            // emitted symbols.
            std::vector<std::string> depObjects;
            for (const Dependency& dep : deps)
                if (!dep.objectPath.empty())
                    depObjects.push_back(dep.objectPath);
            if (!linkExecutable(objectPath, outputPath, depObjects,
                                opts.sanitizeAddress)) {
                std::cerr << "bezelc: link error\n";
                std::remove(objectPath.c_str());
                return CompileResult{false, diagnostics.errorCount() + 1};
            }
            std::remove(objectPath.c_str());
        }
    }

    for (const Diagnostic& diag : diagnostics.diagnostics()) {
        std::cerr << path << ":" << diag.location.line << ":"
                  << diag.location.column << ": " << diag.message << "\n";
    }

    CompileResult result;
    result.errorCount = diagnostics.errorCount();
    result.success = !diagnostics.hasErrors();
    return result;
}

CompileResult Driver::compileObject(const std::string& path,
                                    const std::string& objectPath,
                                    const std::vector<Dependency>& deps) {
    std::string source;
    if (!readFile(path, source)) {
        std::cerr << "bezelc: error: cannot open '" << path << "'\n";
        return CompileResult{false, 1};
    }

    DiagnosticEngine diagnostics;
    Lexer lexer(source, diagnostics);
    Parser parser(lexer, diagnostics);
    auto unit = parser.parseCompilationUnit();

    // §2.6 (build phases) / #1622: do not run the front-end on an error-
    // recovery AST. See compileFile for the full rationale — the same parse-
    // gates-the-front-end rule applies to this library/object path.
    if (unit && !diagnostics.hasErrors()) {
        // stdlib §2.20 (#2047): inject the auto-imported `Result<T, E>` choice
        // (the library/object path mirrors compileFile).
        injectStdlibPrelude(*unit);
        sema::SemaContext semaCtx(diagnostics);
        sema::collectSymbols(*unit, semaCtx);
        // §2.5: a library may itself import dependencies (acyclic). #1487: load
        // them in two global passes (see the compileFile path) so cross-artifact
        // Named references resolve independently of the `--import` order.
        {
            std::vector<std::string> depMetaPaths;
            depMetaPaths.reserve(deps.size());
            for (const Dependency& dep : deps)
                if (!dep.metadataPath.empty())
                    depMetaPaths.push_back(dep.metadataPath);
            // §2.8.4 rule 4 (#1733): reject an executable artifact presented as
            // a dependency — see the compileFile path above for the rationale.
            std::string depErr, failedPath;
            if (!depMetaPaths.empty() &&
                !bezel::module::loadDependencies(depMetaPaths, semaCtx, &depErr,
                                                 &failedPath)) {
                if (!depErr.empty())
                    std::cerr << "bezelc: error: " << depErr << "\n";
                else
                    std::cerr
                        << "bezelc: error: cannot load module metadata from '"
                        << failedPath << "'\n";
                return CompileResult{false, 1};
            }
        }
        sema::resolveTypes(*unit, semaCtx);
        sema::typecheckExpressions(*unit, semaCtx);
        // §22.15 (v0.202, #2052): enforce the pure-body rule set on every
        // pure member. Runs after typecheck so resolved call targets and
        // expression types are available.
        sema::checkPureBodies(*unit, semaCtx);
        sema::checkDefiniteAssignment(*unit, semaCtx);
        sema::checkStaticInitCycles(*unit, semaCtx);  // §2.5 / #797
        ownership::checkOwnership(*unit, semaCtx);

        TypeChecker typeChecker(diagnostics);
        typeChecker.check(*unit);
        OwnershipChecker ownershipChecker(diagnostics);
        ownershipChecker.check(*unit);

        // §33.6: a library has no `Main` requirement — the entry-point check is
        // intentionally NOT run here (that is the executable path's concern).
        if (!diagnostics.hasErrors()) {
            llvm::LLVMContext llvmCtx;
            // per §33.1/§2.4/§33.6 (#1563): this artifact is a library, never
            // the executable's main module (it is either compiled standalone
            // as a library or loaded elsewhere as a dependency, §2.5) — a
            // qualifying `Main` here is an ordinary method, not the native
            // process entry point. Without this, a class's qualifying `Main`
            // was unconditionally lowered to the native `main` symbol even in
            // a library build, so importing this artifact into an executable
            // that has its own `Main` failed to link with a duplicate
            // definition of `main`.
            codegen::CodegenOptions libOpts;
            libOpts.isMainModule = false;
            auto module = codegen::emitModule(*unit, semaCtx, llvmCtx, libOpts);
            // §2.6 step 4: embed the public-interface metadata so an importer's
            // loader can read this object's interface (issue #328 inc 1/2/3).
            // §14 / #662c: pass `source` for ctor/dtor body export.
            if (module) bezel::module::embedModuleMetadata(*module, semaCtx, source);
            std::string err;
            if (!module || !codegen::emitObjectFile(*module, objectPath, err)) {
                // Print codegen diagnostics (e.g. "not yet implemented") before
                // failing (#628 pt 1).
                for (const Diagnostic& diag : diagnostics.diagnostics()) {
                    std::cerr << path << ":" << diag.location.line << ":"
                              << diag.location.column << ": " << diag.message
                              << "\n";
                }
                if (!err.empty())
                    std::cerr << "bezelc: codegen error: " << err << "\n";
                return CompileResult{false, diagnostics.errorCount() + 1};
            }
        }
    }

    for (const Diagnostic& diag : diagnostics.diagnostics()) {
        std::cerr << path << ":" << diag.location.line << ":"
                  << diag.location.column << ": " << diag.message << "\n";
    }

    CompileResult result;
    result.errorCount = diagnostics.errorCount();
    result.success = !diagnostics.hasErrors();
    return result;
}

} // namespace bezel
