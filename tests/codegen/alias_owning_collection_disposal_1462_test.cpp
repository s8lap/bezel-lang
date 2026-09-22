// Codegen/ownership — a TYPE ALIAS to an owning collection must be transparent
// for disposal: `type OA := OwningArray<R>` behaves identically to the
// underlying `OwningArray<R>` (issue #1462, §27.8 / §28.3 / §28.11).
//
// Before the fix, the alias-resolved TypeRef kept the use-site astNode (the
// alias name `OA`) for diagnostics, so every builtin name-head check that
// identifies a collection by `astNode->name->parts[0]` read `OA`, not
// `OwningArray`. Consequences through the alias: the §28.3 producer
// classification missed `a.PopLast()` (the owned element leaked, no auto-drop
// warning), and the codegen disposal/element-finalization paths mis-identified
// the collection. The fix carries the underlying head on the TypeRef
// (`aliasResolvedHead`, read via `TypeRef::headName()`) so sema, ownership, and
// codegen all see through the alias.
//
// Each program uses a class R with a static counting destructor. Cases that
// rely on §28.3 AUTO-DROP are verified through the LeakSanitizer leg, which runs
// the ownership pass + AOT codegen (the in-process runJit harness deliberately
// omits checkOwnership, so it cannot observe auto-drop). A clean LSan run proves
// the element was disposed exactly once — a leak (too few) or a double-free /
// UAF (too many) both fail it. Cases that use EXPLICIT disposal (`using var`)
// also assert the JIT exit code (the dispose count), since that path needs no
// ownership pass.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1462() {
    return wtScratchDir("wt-1462");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    auto dir = wtDir1462();
    std::string stem = unique_stem("bezel_1462_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/UAF
    return 0;
}

// R with a static counting destructor; header shared by every program.
const char* kR =
    "module M; class R public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor R._n := R._n + 1; end destructor; end R; ";

std::string prog(const std::string& aliases, const std::string& body) {
    return std::string(kR) + aliases +
           " class C public static method Main(): int " + body +
           " end Main; end C;";
}

// §28.3 auto-drop of an owned element popped from an ALIASED OwningArray.
// `x := a.PopLast()` is an owned transfer; x is undischarged → auto-drop
// disposes it once at scope end. Observed after a nested scope exits.
BEZEL_TEST(codegen_alias_owning_disposal_1462, aliased_owningarray_poplast_autodrop) {
    std::string src = std::string(kR) +
        "type OA := OwningArray<R>; "
        "class C "
        "  static method f() using var a: OA := new OwningArray<R>(); "
        "    a.Add(new R()); var x := a.PopLast(); end f; "
        "  public static method Main(): int f(); return R._n; end Main; end C;";
    int lsan = runUnderLeakSanitizer(src);  // runs checkOwnership + AOT
    if (lsan == -1) return;                 // toolchain unavailable
    BEZEL_EXPECT_EQ(lsan, 0);               // disposed exactly once
}

// Aliased OwningStack.Pop and OwningQueue.Dequeue are owned transfers too.
BEZEL_TEST(codegen_alias_owning_disposal_1462, aliased_stack_pop_and_queue_dequeue) {
    std::string src = std::string(kR) +
        "type OStk := OwningStack<R>; type OQ := OwningQueue<R>; "
        "class C "
        "  static method fs() using var s: OStk := new OwningStack<R>(); "
        "    s.Push(new R()); var x := s.Pop(); end fs; "
        "  static method fq() using var q: OQ := new OwningQueue<R>(); "
        "    q.Enqueue(new R()); var y := q.Dequeue(); end fq; "
        "  public static method Main(): int fs(); fq(); return R._n; end Main; end C;";
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Aliased OwningArray.Drain() for-in disposes every drained element (each bound
// to `using var _`). Before the fix only some were disposed (the iterator /
// element-finalization path was alias-blind).
BEZEL_TEST(codegen_alias_owning_disposal_1462, aliased_drain_for_in_disposes_all) {
    std::string src = std::string(kR) +
        "type OA := OwningArray<R>; "
        "class C "
        "  static method f() using var a: OA := new OwningArray<R>(); "
        "    a.Add(new R()); a.Add(new R()); a.Add(new R()); "
        "    for var e in a.Drain() do using var _ := e; end; end f; "
        "  public static method Main(): int f(); return R._n; end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// Aliased container element-finalization: `using var` over an aliased owning
// collection disposes the elements it still holds at scope end.
BEZEL_TEST(codegen_alias_owning_disposal_1462, aliased_container_finalizes_elements) {
    std::string src = std::string(kR) +
        "type OSet := OwningSet<R>; type OLL := OwningLinkedList<R>; "
        "class C "
        "  static method fs() using var s: OSet := new OwningSet<R>(); "
        "    s.Add(new R()); s.Add(new R()); end fs; "
        "  static method fl() using var l: OLL := new OwningLinkedList<R>(); "
        "    l.AddLast(new R()); l.AddLast(new R()); end fl; "
        "  public static method Main(): int fs(); fl(); return R._n; end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 4);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// A CHAINED alias (`type A := B := OwningArray<R>`) resolves through both hops.
BEZEL_TEST(codegen_alias_owning_disposal_1462, chained_alias_resolves) {
    std::string src = std::string(kR) +
        "type Inner := OwningArray<R>; type Outer := Inner; "
        "class C "
        "  static method f() using var a: Outer := new OwningArray<R>(); "
        "    a.Add(new R()); var x := a.PopLast(); end f; "
        "  public static method Main(): int f(); return R._n; end Main; end C;";
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Equivalence: the direct (non-aliased) form disposes identically — a guard
// that the alias path neither under- nor over-disposes relative to baseline.
BEZEL_TEST(codegen_alias_owning_disposal_1462, direct_form_unchanged_baseline) {
    std::string src = std::string(kR) +
        "class C "
        "  static method f() using var a: OwningArray<R> := new OwningArray<R>(); "
        "    a.Add(new R()); var x := a.PopLast(); end f; "
        "  public static method Main(): int f(); return R._n; end Main; end C;";
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace
