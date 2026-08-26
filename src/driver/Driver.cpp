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
#include "driver/ScopedAwaitBind.hpp"
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

// stdlib §7.1 / §7.2 (issue #645): the System.IO enums. Like `Result` above,
// these are pure Bezel declarations with no compiler intrinsic — every rule
// that applies to a user enum (§10.2 auto-increment values, §10.6 `case`
// matching with prefix omission, §19.6 exhaustiveness, the underlying-type
// casts of §5.2) applies to them unchanged — so they are made available by
// injecting their VERBATIM stdlib source rather than by hand-registering a
// builtin. The declaration order fixes the ordinals the FileStream runtime
// consumes (FileMode.CreateNew = 0 … Append = 5, etc.), so this source and
// bz_fs_open_flags in bezel_runtime_fileio.cpp must stay in step.
//
// The enums are always available rather than gated on `import System.IO;` —
// the same stance the compiler already takes for the other §7 types
// (File/Directory/Path resolve without the import today; import-gating is a
// separate, uniform piece of work).
const char kIoPreludeSource[] =
    "module __bezel_prelude_io__;\n"
    "public enum SeekOrigin: byte\n"
    "  Begin, Current, End;\n"
    "end SeekOrigin;\n"
    "\n"
    "public enum FileMode: byte\n"
    "  CreateNew, Create, Open, OpenOrCreate, Truncate, Append;\n"
    "end FileMode;\n"
    "\n"
    "public enum FileAccess: byte\n"
    "  Read, Write, ReadWrite;\n"
    "end FileAccess;\n";

// Parse ONE auto-imported stdlib prelude source and PREPEND its top-level
// declarations to `unit`. The prelude sources are program-lifetime constants,
// so the AST string_views into them stay valid for the whole compilation;
// ownership of the parsed nodes transfers into `unit->topDecls`. A private
// diagnostics sink absorbs any prelude parse diagnostic (the prelude is
// known-valid — a diagnostic here would be an internal compiler defect, never
// the user's fault).
void injectOnePrelude(ast::SourceFile& unit, const std::string& preludeSrc) {
    DiagnosticEngine preludeDiags;
    Lexer preludeLexer(preludeSrc, preludeDiags);
    Parser preludeParser(preludeLexer, preludeDiags);
    std::unique_ptr<ast::SourceFile> preludeUnit =
        preludeParser.parseCompilationUnit();
    if (!preludeUnit || preludeDiags.hasErrors()) return;  // defensive: no-op
    // #2047 follow-up 2 / #2131 / §2.3 v0.226: an auto-imported prelude type
    // is SHADOWED by a user top-level declaration of the same name (standard
    // auto-import shadowing — §2.3's paragraph "the other always-available
    // names... may be shadowed by local declarations under the normal
    // name-resolution rules"). Inject a prelude decl ONLY when the user
    // declares no type of that name. Without this, a program that declares its
    // own `Result` (the corpus does — e.g. a `closed class Result<T>`
    // hierarchy) collides with the injected `Result<T, E>` choice: two
    // same-named top-level types corrupt name resolution and
    // per-monomorphization descriptor emission for the WHOLE module (an
    // unrelated `is Box<string>` then mis-resolves). The user's type wins; the
    // stdlib `Result` is simply unavailable in that module. Pinned by
    // tests/codegen/result_stdlib_2047_test.cpp (spec_2_20_user_result_type_*).
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

// Inject every auto-imported stdlib prelude source into `unit`.
void injectStdlibPrelude(ast::SourceFile& unit) {
    static const std::string resultSrc(kResultPreludeSource);
    static const std::string ioSrc(kIoPreludeSource);
    injectOnePrelude(unit, resultSrc);   // stdlib §2.20 Result<T, E>
    injectOnePrelude(unit, ioSrc);       // stdlib §7.1/§7.2 System.IO enums
}

// Proposal 0082 §3 stage-1 PROTOTYPE (#3048), reached only when
// `CodegenOptions::scopedAwaitBind` is set — off by default, so with the flag
// off not one instruction of this runs and the pipeline below is bit-for-bit
// what it was.
//
// Collect the sites the scoped await-bind would desugar. The set is NOT
// computed by any new classification: it is exactly the set of `await`
// locations at which the UNMODIFIED ownership pass reports the §30.5 Pattern A
// consumer-side rejection. The front-end is therefore run once over a
// THROWAWAY DiagnosticEngine (nothing is printed and nothing is kept but the
// locations); the real compilation then proceeds over the rewritten AST with
// its own engine, so a program's diagnostics are unaffected by this pass ever
// having run. See src/driver/ScopedAwaitBind.hpp for why the prototype is
// shaped this way (it is the C3 evidence).
//
// The phase sequence below mirrors compileExecutableObject's verbatim, so the
// classification run cannot diverge from the run whose diagnostic it reads.
std::vector<SourceLocation> collectScopedAwaitBindSites(
    const std::string& source, const std::vector<Dependency>& deps) {
    std::vector<SourceLocation> sites;

    DiagnosticEngine scratch;
    Lexer lexer(source, scratch);
    Parser parser(lexer, scratch);
    std::unique_ptr<ast::SourceFile> unit = parser.parseCompilationUnit();
    if (!unit || scratch.hasErrors()) return sites;   // parse gates the front-end

    injectStdlibPrelude(*unit);
    sema::SemaContext ctx(scratch);
    sema::collectSymbols(*unit, ctx);
    {
        std::vector<std::string> depMetaPaths;
        depMetaPaths.reserve(deps.size());
        for (const Dependency& dep : deps)
            if (!dep.metadataPath.empty())
                depMetaPaths.push_back(dep.metadataPath);
        std::string depErr, failedPath;
        // Use the FULL dotted name — ctx.module()->name is the leaf.
        std::string rootDottedName;
        if (unit->module && unit->module->name) {
            for (std::size_t i = 0; i < unit->module->name->parts.size(); ++i) {
                if (i != 0) rootDottedName += '.';
                rootDottedName += std::string(unit->module->name->parts[i]);
            }
        }
        std::string_view rootName = rootDottedName;
        if (!depMetaPaths.empty() &&
            !bezel::module::loadDependencies(depMetaPaths, ctx, &depErr,
                                             &failedPath, rootName))
            return sites;   // the real run reports it; stay silent here
    }
    sema::resolveTypes(*unit, ctx);
    sema::typecheckExpressions(*unit, ctx);
    sema::checkPureBodies(*unit, ctx);
    sema::checkDefiniteAssignment(*unit, ctx);
    sema::checkStaticInitCycles(*unit, ctx);
    ownership::checkOwnership(*unit, ctx);

    for (const Diagnostic& d : scratch.diagnostics()) {
        if (d.severity != Severity::Error) continue;
        if (d.message.rfind(proto::kPatternARejectPrefix, 0) == 0)
            sites.push_back(d.location);
    }
    return sites;
}

// The prototype's single entry point into the real pipeline: rewrite `unit` in
// place when the flag is on. A no-op — not even a parse — when it is off.
void maybeDesugarScopedAwaitBinds(bool enabled, const std::string& source,
                                  const std::vector<Dependency>& deps,
                                  ast::SourceFile& unit) {
    if (!enabled) return;
    proto::rewriteScopedAwaitBinds(unit,
                                   collectScopedAwaitBindSites(source, deps));
}

} // namespace

// Every auto-imported stdlib prelude source, concatenated — exactly the text
// injectStdlibPrelude parses into EVERY compilation unit.
//
// Exposed for the AOT object cache's key. The prelude is an INPUT to codegen:
// the same user source compiled against two different preludes can produce two
// different objects, so a cache that did not key on it could serve an object
// built against a stale prelude — a wrong binary reported as a pass. Today
// these sources are `const char[]` compiled into bezel_core, so the compiler-
// archive identity already covers them; but that is an incidental property of
// where the text happens to LIVE, and a refactor that loaded the stdlib from
// disk (a plausible direction) would silently remove the coverage while every
// test kept passing. Keying on the text itself makes the guarantee local,
// permanent, and testable instead of incidental.
//
// The NUL separator keeps the concatenation unambiguous: no shuffling of text
// between two adjacent preludes can produce the same fingerprint.
std::string Driver::preludeSourceFingerprint() {
    std::string s;
    s += kResultPreludeSource;   // stdlib §2.20 Result<T, E>
    s += '\0';
    s += kIoPreludeSource;       // stdlib §7.1/§7.2 System.IO enums
    return s;
}

// The on-disk Bezel runtime archive the link consumes, or "" when CMake did not
// inject a path. Exposed so a caller that must reason about the link's INPUTS
// (the AOT test harness's link cache) reads the same value the link itself uses,
// rather than guessing.
std::string Driver::runtimeLibraryPath() {
#ifdef BEZEL_RUNTIME_LIB_PATH
    return BEZEL_RUNTIME_LIB_PATH;
#else
    return {};
#endif
}

// The EXACT clang command `linkExecutableObject` runs. Factored out (rather than
// built inline in the link) so the recipe has exactly ONE definition: a caller
// that needs to know what the link depends on — the AOT test harness, which
// hashes the recipe into its link-cache key — derives it from this function, so
// any future edit to a flag, a library, or their order is picked up
// automatically and cannot drift from what actually runs.
std::string Driver::linkCommandLine(const std::string& objectPath,
                                    const std::string& exePath,
                                    const std::vector<std::string>& extraObjects,
                                    bool sanitizeAddress,
                                    const std::string& runtimeLibPath) {
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
    if (!runtimeLibPath.empty()) {
        // Static-link the Bezel runtime (path injected at compile time by CMake).
        cmd += " ";
        cmd += runtimeLibPath;
    }
    cmd += " -lstdc++";  // runtime uses C++ std (std::mutex, std::thread, etc.)
    return cmd;
}

// A path-independent fingerprint of the link recipe, for cache keying. It is the
// real command line with its three PATH inputs replaced by placeholders:
//   * the object and executable paths are per-invocation temporaries — the
//     linked bytes do not depend on them, and keying on them would defeat any
//     cache outright;
//   * the runtime archive's PATH is abstracted because a caller keys on the
//     archive's CONTENT (its hash) instead — that is the load-bearing input, and
//     abstracting the path lets two checkouts with byte-identical runtimes share
//     entries. Whether a runtime is linked AT ALL is still visible (the
//     placeholder is absent when no path was injected).
// Everything else — the compiler name, every flag, every library, and their
// ORDER — appears verbatim, so any recipe change changes this string.
std::string Driver::linkRecipeSignature(bool sanitizeAddress) {
    const std::string rt = runtimeLibraryPath();
    return linkCommandLine("<OBJ>", "<EXE>", /*extraObjects=*/{}, sanitizeAddress,
                           rt.empty() ? std::string() : std::string("<RUNTIME>"));
}

// Link a native object (plus any §2.5 dependency objects) into an executable
// using clang (the project's toolchain). Returns true on success. The
// dependency objects supply the definitions of the imported symbols the
// importer's object references as externals (issue #328 inc 3).
bool Driver::linkExecutableObject(const std::string& objectPath,
                                  const std::string& exePath,
                                  const std::vector<Dependency>& deps,
                                  const codegen::CodegenOptions& opts) {
    std::vector<std::string> depObjects;
    for (const Dependency& dep : deps)
        if (!dep.objectPath.empty()) depObjects.push_back(dep.objectPath);
    const std::string cmd = linkCommandLine(objectPath, exePath, depObjects,
                                            opts.sanitizeAddress,
                                            runtimeLibraryPath());
    if (std::system(cmd.c_str()) == 0) return true;
    std::cerr << "bezelc: link error\n";
    return false;
}

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

// compileFile is EXACTLY the composition of its two halves — compile the
// executable-mode object, then link it — so the shipping AOT path and the split
// entry points can never drift: there is one front-end (compileExecutableObject)
// and one link recipe (linkExecutableObject), and this is their only caller
// ordering. The object is a build temporary here, removed on both exits, so a
// bezelc invocation is byte-for-byte what it was when the two halves were one
// function.
//
// The one observable difference from the pre-split code is on the LINK-FAILURE
// path: warnings recorded by a clean front-end are now printed (by
// compileExecutableObject) BEFORE "bezelc: link error", where previously the
// early return skipped them. A failing link is an infrastructure failure, not a
// language-level outcome, and no caller inspects that ordering.
CompileResult Driver::compileFile(const std::string& path,
                                  const std::string& outputPath,
                                  const std::vector<Dependency>& deps,
                                  const codegen::CodegenOptions& opts) {
    // An empty outputPath is the front-end-only call (no codegen, no §33.5
    // entry-point check): compileExecutableObject with no object to write.
    if (outputPath.empty())
        return compileExecutableObject(path, /*objectPath=*/"", deps, opts);

    const std::string objectPath = outputPath + ".o";
    // §2.8.1 rule 3 (#1729, v0.188): the loader's metadata-side dedup
    // collapses bit-identical duplicates of one moduleName (the diamond-
    // assembly shape). The link step still sees every dependency object
    // unless we filter the `deps` to the dedup'd set — otherwise the
    // dedupped second copy is double-linked and fails with "multiple
    // definition of …". Compile-time only; the surviving paths are
    // thrown away after the link.
    std::vector<std::string> survivingDepPaths;
    CompileResult cc = compileExecutableObject(path, objectPath, deps, opts,
                                               &survivingDepPaths);
    if (!cc.success) return cc;
    // Build the filtered-deps list: keep only deps whose metadataPath
    // survived the loader's dedup, AND dedup by metadataPath so two deps
    // pointing at the same metadata (which is also the typical diamond
    // shape — App.bzl imports Base twice, once via LibA and once via LibB)
    // link only the canonical copy. An empty `survivingDepPaths` means
    // the caller did not opt in to the dedup reporting (older test
    // harnesses, the front-end-only path); fall back to the original
    // deps — the dedup simply cannot reach the link in that case, which
    // matches the pre-#1729 behavior.
    std::vector<Dependency> linkDeps;
    if (!survivingDepPaths.empty()) {
        std::unordered_set<std::string> survivors(survivingDepPaths.begin(),
                                                  survivingDepPaths.end());
        std::unordered_set<std::string> seenMeta;
        linkDeps.reserve(deps.size());
        for (const Dependency& dep : deps) {
            // Empty metadataPath = no loader work; pass the dep through.
            // (Such deps have no module to dedup; they typically carry an
            // object-only entry to link against.)
            if (dep.metadataPath.empty()) {
                linkDeps.push_back(dep);
                continue;
            }
            if (!survivors.count(dep.metadataPath)) continue;
            // Among the surviving deps, dedup by metadataPath so a diamond
            // that names the same metadata file twice links only once.
            if (!seenMeta.insert(dep.metadataPath).second) continue;
            linkDeps.push_back(dep);
        }
    }
    if (!linkExecutableObject(objectPath, outputPath,
                              linkDeps.empty() ? deps : linkDeps, opts)) {
        std::remove(objectPath.c_str());
        return CompileResult{false, cc.errorCount + 1};
    }
    std::remove(objectPath.c_str());
    return cc;
}

// The FRONT-END half of the executable build: everything compileFile does up to
// and including `emitObjectFile`, leaving the native object at `objectPath` and
// performing NO link. An empty `objectPath` runs the front-end only (no codegen,
// no §33.5 entry-point check) — the historical `compileFile(path, "")` shape.
//
// Split out for the AOT test harness's two-level cache: the emitted object is a
// pure function of (this compiler's code, `opts`, the source text) — codegen
// includes no runtime header and never reads the runtime archive; the runtime is
// consumed by the LINK alone (linkCommandLine). Keying the two halves separately
// therefore lets a runtime-only rebuild keep cached objects valid, and a
// codegen-only rebuild keep cached LINK products valid for every object that
// came out unchanged.
CompileResult Driver::compileExecutableObject(
    const std::string& path, const std::string& objectPath,
    const std::vector<Dependency>& deps, const codegen::CodegenOptions& opts,
    std::vector<std::string>* survivingDepPaths) {
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
        // Proposal 0082 §3 stage-1 PROTOTYPE (#3048), OFF BY DEFAULT: rewrite
        // `var x := await <fresh Task<T> producer>;` into the two-line form
        // `using var $t := <producer>; var x := await $t;` — a source-level
        // macro expansion into today's language, so every pass below runs
        // UNMODIFIED over an ordinary `using var` (§28.10). With the flag off
        // this call returns immediately and the pipeline is unchanged.
        maybeDesugarScopedAwaitBinds(opts.scopedAwaitBind, source, deps, *unit);
        // stdlib §2.20 (#2047): make the auto-imported `Result<T, E>` choice
        // available by injecting its synthesized declaration before symbol
        // collection, so user references to `Result<T, E>` resolve.
        injectStdlibPrelude(*unit);
        // Sema milestone — runs in pass order: symbol collection,
        // type resolution, expression typecheck. Each pass annotates
        // the AST via side-tables on SemaContext.
        sema::SemaContext semaCtx(diagnostics);
        sema::collectSymbols(*unit, semaCtx);
        // §2.8.1 rule 5 (#1729, v0.188): the root compilation's module name
        // must not collide with an implementation-provided stdlib module.
        // Detected here (after symbol collection, where the ModuleDecl's
        // name is attached to the SemaContext) and BEFORE the loader runs so
        // the diagnostic names the user's own source. §2.8.1 rules 3+5:
        // stdlib modules cannot be re-declared by user code.
        //
        // The SemaContext's ModuleSymbol carries the LEAF name only (the
        // short identifier, not the dotted form — moduleSymbolName in
        // SymbolCollection.cpp takes parts.back()); the impl-provided set
        // uses the full dotted form, so we must reconstruct the full name
        // from the AST's QualifiedName. Mirrors the §2.8.1 rule 4 self-
        // import reconstruction in TypeResolution.cpp's sourceImports loop.
        std::string rootDottedName;
        if (unit->module && unit->module->name) {
            for (std::size_t i = 0; i < unit->module->name->parts.size(); ++i) {
                if (i != 0) rootDottedName += '.';
                rootDottedName += std::string(unit->module->name->parts[i]);
            }
        }
        if (!rootDottedName.empty() &&
            bezel::module::isImplProvidedModuleName(rootDottedName)) {
            std::cerr << "bezelc: error: source file '" << path
                      << "' declares module '" << rootDottedName
                      << "', which is an implementation-provided module name "
                         "(§2.8.1 rules 3+5); stdlib modules cannot be "
                         "re-declared by user code\n";
            return CompileResult{false, 1};
        }
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
        //
        // §2.8.1 rules 3+5 (#1729, v0.188): the loader receives the root's
        // module name so it can reject a dep that re-declares it (rule 3's
        // "dep declaring a name the root also declares") or one of the
        // stdlib's impl-provided names (rule 5). `survivingDepPaths` collects
        // the dedup'd set so compileFile can pass exactly those to the link
        // step (otherwise a diamond-assembly duplicate fails the link).
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
            std::vector<std::string> depSurvivors;
            // Use the full dotted root name (not the leaf on semaCtx.module()
            // — see the impl-provided check above for why) so the loader
            // rejects a dep whose moduleName equals the root's FULL name
            // and so the impl-provided-name comparison in loadDependencies
            // sees the dotted form.
            std::string_view rootName = rootDottedName;
            if (!depMetaPaths.empty() &&
                !bezel::module::loadDependencies(
                    depMetaPaths, semaCtx, &depErr, &failedPath, rootName,
                    survivingDepPaths ? survivingDepPaths : &depSurvivors)) {
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
        // Scoped to the executable path (non-empty objectPath) so library
        // compilation and the sema/JIT test harnesses — which run the shared
        // front-end on snippets that legitimately omit `Main` (§33.6) — are
        // not failed by this requirement. Only run when the front-end is
        // otherwise clean, to avoid cascading on already-broken programs.
        if (!objectPath.empty() && !diagnostics.hasErrors()) {
            sema::checkEntryPoint(*unit, semaCtx);
        }

        // Codegen: when an object path is requested and the front-end is
        // clean, lower to LLVM and emit a native object. (The link is the
        // caller's step — compileFile's second half.)
        if (!objectPath.empty() && !diagnostics.hasErrors()) {
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
                // (non-empty objectPath, entry-point-checked above) — record
                // the artifact as executable-kind so an importer rejects it as a
                // dependency (§2.8.4 rule 4).
                bezel::module::embedModuleMetadata(*module, semaCtx, source,
                                                   /*isExecutable=*/true, path);
            }
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
                                    const std::vector<Dependency>& deps,
                                    const codegen::CodegenOptions& opts) {
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
        // Proposal 0082 §3 stage-1 PROTOTYPE (#3048), OFF BY DEFAULT — the
        // library/object path mirrors compileExecutableObject. `opts` is read
        // for this ONE bit here; the library build's codegen options stay the
        // locally-built `libOpts` below, so no other flag's meaning changes on
        // this path.
        maybeDesugarScopedAwaitBinds(opts.scopedAwaitBind, source, deps, *unit);
        // stdlib §2.20 (#2047): inject the auto-imported `Result<T, E>` choice
        // (the library/object path mirrors compileFile).
        injectStdlibPrelude(*unit);
        sema::SemaContext semaCtx(diagnostics);
        sema::collectSymbols(*unit, semaCtx);
        // §2.8.1 rule 5 (#1729, v0.188): impl-provided-name check (mirrors
        // compileExecutableObject). The SemaContext's ModuleSymbol carries
        // the leaf only — reconstruct the full dotted name from the AST.
        std::string rootDottedName;
        if (unit->module && unit->module->name) {
            for (std::size_t i = 0; i < unit->module->name->parts.size(); ++i) {
                if (i != 0) rootDottedName += '.';
                rootDottedName += std::string(unit->module->name->parts[i]);
            }
        }
        if (!rootDottedName.empty() &&
            bezel::module::isImplProvidedModuleName(rootDottedName)) {
            std::cerr << "bezelc: error: source file '" << path
                      << "' declares module '" << rootDottedName
                      << "', which is an implementation-provided module name "
                         "(§2.8.1 rules 3+5); stdlib modules cannot be "
                         "re-declared by user code\n";
            return CompileResult{false, 1};
        }
        // §2.5: a library may itself import dependencies (acyclic). #1487: load
        // them in two global passes (see the compileFile path) so cross-artifact
        // Named references resolve independently of the `--import` order.
        // §2.8.1 rules 3+5 (#1729, v0.188): pass the root name so the loader
        // can reject a dep that re-declares it or declares an impl-provided
        // name. compileObject does no linking, so no survivingPaths needed.
        {
            std::vector<std::string> depMetaPaths;
            depMetaPaths.reserve(deps.size());
            for (const Dependency& dep : deps)
                if (!dep.metadataPath.empty())
                    depMetaPaths.push_back(dep.metadataPath);
            // §2.8.4 rule 4 (#1733): reject an executable artifact presented as
            // a dependency — see the compileFile path above for the rationale.
            std::string depErr, failedPath;
            std::string_view rootName = rootDottedName;
            if (!depMetaPaths.empty() &&
                !bezel::module::loadDependencies(depMetaPaths, semaCtx, &depErr,
                                                 &failedPath, rootName)) {
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
            if (module)
                bezel::module::embedModuleMetadata(*module, semaCtx, source,
                                                   /*isExecutable=*/false, path);
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
