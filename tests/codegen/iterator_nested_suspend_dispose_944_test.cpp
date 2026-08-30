// Codegen — #944 / §29.7: when an OUTER iterator is abandoned (Dispose called
// because the consumer broke out / returned) while SUSPENDED at a `yield` INSIDE
// an inner `for-in` over another iterator, the inner enumerator (and, for a
// loop-owned source temporary, the source IEnumERABLE) active at the suspension
// point must be disposed — which in turn runs the inner iterator's own Dispose,
// disposing its active `using var`s.
//
// Root cause (pre-fix): the inner for-in's implicit enumerator (the
// GetEnumerator clone, §29.11) and the loop-owned source temporary are owned
// resources scoped to the loop body, exactly like a `using var`, but they were
// NOT registered among the resources the enclosing iterator's state-machine
// Dispose runs for a suspend state spanned by the loop. So abandoning the outer
// iterator mid-inner-loop ran the outer finally but skipped the inner
// enumerator/source — the inner iterator's active `using var` (and the source
// frame itself) leaked. §29.7: Dispose "Calls Dispose() on every using var
// active at the point of suspension."
//
// Fix: collectSuspendLocals reserves dedicated frame fields for such a for-in's
// enumerator and loop-owned source; the lowering wires them through those fields
// and records each live frame's cleanup action per suspend state
// (smIterCleanupAtState_, issue 3579); the iterator Dispose, for a suspend
// state inside the loop, GEPs the field, loads,
// null-guards, dispatches the iterator-vtable slot-1 Dispose, then nulls the
// field. Exactly once: the loop's OWN scope-exit / break disposal handles every
// other exit (never both — the suspend state discriminates).
//
// These tests run the FULL pipeline (including the ownership pass) so the
// scope-end / loop / suspension disposals are all emitted. The Cell counter is
// the leak detector: a leak leaves Cell.n > 0, a double-dispose drives it
// negative; both surface as a non-zero return. ASan double-free/UAF freedom is
// checked independently in the lane's manual verification.

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
// Full-pipeline runner (mirrors iterator_reenum_943_test): runs the ownership
// pass before codegen so scope-end / loop / suspension disposals are emitted,
// then JIT-executes `main` and returns its value.
int run(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    bezel::test::ensureNativeTargetInitialized();
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return -999;
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return -999; }
    auto jit = std::move(*jitOrErr);
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    auto fn = sym->toPtr<int (*)()>();
    return fn();
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// §29.7: the issue's repro — breaking out of the outer iterator while it is
// suspended inside `for var x in P.Inner()` must dispose the inner enumerator
// (whose Dispose disposes Inner's active `using var c1`). Cell.n nets to 0.
BEZEL_TEST(iterator_nested_suspend_dispose_944, break_out_disposes_inner_using_var) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<Cell> "
        "    using var c1 := new Cell(); yield return c1; "
        "    using var c2 := new Cell(); yield return c2; end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    for var x in P.Inner() do yield return 1; end; end Outer; "
        "  public static method Main(): int "
        "    for var x in P.Outer() do break; end; "
        "    return Cell.n; "  // 0 — c1 disposed via the inner enumerator's Dispose
        "  end Main; end P;"), 0);
}

// §29.7: NORMAL completion (consume all of Outer, no break) — the inner
// enumerator/source are disposed at the loop's own exit, NOT also by the
// state-machine Dispose. No leak, no double-free → Cell.n == 0.
BEZEL_TEST(iterator_nested_suspend_dispose_944, normal_completion_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<Cell> "
        "    using var c1 := new Cell(); yield return c1; "
        "    using var c2 := new Cell(); yield return c2; end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    for var x in P.Inner() do yield return 1; end; end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var x in P.Outer() do total := total + x; end; "  // consume all
        "    return Cell.n + (total - 2); "  // Cell.n=0, total=2 → 0
        "  end Main; end P;"), 0);
}

// §29.7: MULTIPLE inner `using var`s active at the suspension point — abandoning
// the outer iterator while suspended at the first inner yield (with both `a` and
// `b` live) must dispose BOTH (Cell.n nets to 0), not just one.
BEZEL_TEST(iterator_nested_suspend_dispose_944, multiple_inner_using_vars_all_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<int> "
        "    using var a := new Cell(); using var b := new Cell(); "
        "    yield return 1; "                 // suspend with a AND b active
        "    using var c := new Cell(); yield return 2; end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    for var x in P.Inner() do yield return x; end; end Outer; "
        "  public static method Main(): int "
        "    for var x in P.Outer() do break; end; "  // abandon: a+b active
        "    return Cell.n; "  // 0 — both disposed
        "  end Main; end P;"), 0);
}

// §29.7: THREE-level nesting (Outer→Mid→Inner) abandoned at the DEEPEST yield —
// every level's enumerator/source AND each level's active `using var` (o1, m1,
// c1) must be disposed exactly once. Cell.n nets to 0.
BEZEL_TEST(iterator_nested_suspend_dispose_944, three_level_nesting_all_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<int> "
        "    using var c1 := new Cell(); yield return 1; end Inner; "
        "  public static method Mid(): IEnumerable<int> "
        "    using var m1 := new Cell(); "
        "    for var x in P.Inner() do yield return x; end; end Mid; "
        "  public static method Outer(): IEnumerable<int> "
        "    using var o1 := new Cell(); "
        "    for var x in P.Mid() do yield return x; end; end Outer; "
        "  public static method Main(): int "
        "    for var x in P.Outer() do break; end; "  // abandon at deepest yield
        "    return Cell.n; "  // 0 — o1, m1, c1 all disposed
        "  end Main; end P;"), 0);
}

// §29.7: abandoning the outer iterator at a yield BEFORE it enters the inner
// for-in (no enumerator live yet) leaves nothing to dispose — and the later
// inner enumerator field stays null in Dispose (null-guarded). Cell.n == 0.
BEZEL_TEST(iterator_nested_suspend_dispose_944, break_before_inner_loop_starts) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<Cell> "
        "    using var c1 := new Cell(); yield return c1; end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    yield return 99; "                // suspend BEFORE the inner for-in
        "    for var x in P.Inner() do yield return 1; end; end Outer; "
        "  public static method Main(): int "
        "    for var x in P.Outer() do break; end; "  // break at pre-inner yield
        "    return Cell.n; "  // 0 — nothing was allocated
        "  end Main; end P;"), 0);
}

// §29.7 / §29.11: a NAMED-LOCAL source (disposed at its own scope end, NOT
// loop-owned) must be disposed exactly once when the outer iterator is abandoned
// mid-loop — the for-in must NOT also dispose it (that would double-free, driving
// Cell.n negative). Cell.n nets to 0.
BEZEL_TEST(iterator_nested_suspend_dispose_944, named_local_source_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Inner(): IEnumerable<Cell> "
        "    using var c1 := new Cell(); yield return c1; end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    var src := P.Inner(); "           // named-local source
        "    for var x in src do yield return 1; end; end Outer; "
        "  public static method Main(): int "
        "    for var x in P.Outer() do break; end; "  // abandon mid-loop
        "    return Cell.n; "  // 0 — c1 + the named source each disposed once
        "  end Main; end P;"), 0);
}

// §29.11 regression: the #943 re-enumeration still works (two `for` loops over
// the same source each replay the full sequence). The #944 frame-field wiring
// must not disturb the normal loop-exit disposal.
BEZEL_TEST(iterator_nested_suspend_dispose_944, reenum_943_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; end Seq; "
        "  public static method Main(): int "
        "    var s := P.Seq(); "
        "    var s1 := 0; for var x in s do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in s do s2 := s2 + x; end; "
        "    return s1 + s2; "  // 6 + 6 = 12
        "  end Main; end P;"), 12);
}
