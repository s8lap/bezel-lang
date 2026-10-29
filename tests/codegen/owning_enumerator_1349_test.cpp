// #1349 / §29.1 v0.161: IOwningEnumerator<T>.TakeCurrent() replaces the
// destructive `Current` property on owning enumerators.  Protocol misuse
// (double-take without intervening MoveNext, or take before first advance /
// after exhaustion) raises InvalidOperationException.
//
// Tests:
//   1. Manual owning-protocol loop (source/GetOwningEnumerator/MoveNext/TakeCurrent)
//      produces the correct values.
//   2. Double-take (without MoveNext) raises InvalidOperationException.
//   3. Take-before-advance (before the first MoveNext) raises IOE.
//   4. Take-after-exhaustion (after MoveNext returned false) raises IOE.
//   5. for-loop over IOwningEnumerable<T> still works (the for-desugaring calls
//      TakeCurrent internally — no regression from §29.2 for-loop path).
//   6. Disposal is exactly-once (ASan; no leak, no double-free).
//
// Manual-protocol note: in `using var source := Gen(); using var en :=
// source.GetOwningEnumerator()`, the source IOwningEnumerable must be held in a
// using var separately so its owned parameters are disposed (§29.8). The tests
// use this two-var pattern throughout.
//
// Spec citations: §29.1 v0.161, §29.2 v0.161.

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
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {
namespace fs = std::filesystem;

// JIT run without ownership pass (value tests).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Full pipeline (includes DA + ownership) — needed when auto-drop flags matter.
int runFull(const std::string& src) {
    bezel::test::ensureNativeTargetInitialized();
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx);
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
        llvm::consumeError(std::move(err)); return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    int exitCode = sym->toPtr<int(*)()>()();
    bz_flush_static_owned();
    return exitCode;
}

// AOT compile → object → link with ASan → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1349_asan");
}

// Shared counting class for disposal tracking.
const std::string kCountR =
    "class R "
    "  public static _n: int := 0; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

// Shared Box class for value tracking.
const std::string kBox =
    "sealed class Box "
    "  public v: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "end Box; ";

}  // namespace

// ===========================================================================
// §29.1 / #1349: Manual owning-protocol loop value-correct
// ===========================================================================

// Manual protocol: source / GetOwningEnumerator / MoveNext / TakeCurrent.
// Gen() yields Box(1), Box(2), Box(3).  Manual loop sums v fields.
// Result: 1+2+3 = 6.
// Source held in a separate using var (§29.8: source must be disposed so its
// owned parameters — none here, but pattern is correct — are freed).
BEZEL_TEST(owning_enumerator_1349, manual_protocol_values_correct_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "    yield return new Box(2); "
        "    yield return new Box(3); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var s := 0; "
        "    while en.MoveNext() do "
        "      using var x := en.TakeCurrent(); "
        "      s := s + x.v; "
        "    end; "
        "    return s; "  // 1+2+3 = 6
        "  end Main; end C;"), 6);
}

// Manual protocol single element: MoveNext → true, TakeCurrent → value.
BEZEL_TEST(owning_enumerator_1349, manual_protocol_single_element_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(42); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      return x.v; "  // 42
        "    end; "
        "    return -1; "
        "  end Main; end C;"), 42);
}

// ===========================================================================
// §29.1 / #1349: IOE on protocol misuse
// ===========================================================================

// Double-take: MoveNext → TakeCurrent → TakeCurrent (without MoveNext) → IOE.
// Encode: caught = 1 (IOE raised), uncaught = 0 → return 1.
BEZEL_TEST(owning_enumerator_1349, double_take_raises_ioe_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var ok := en.MoveNext(); "
        "    if ok then "
        "      using var first := en.TakeCurrent(); "  // ok: owns element
        "      var caught := 0; "
        "      try "
        "        using var second := en.TakeCurrent(); "  // double-take → IOE
        "        caught := 0; "
        "      catch InvalidOperationException => caught := 1; end; "
        "      return caught; "  // 1 = IOE was raised
        "    end; "
        "    return -1; "
        "  end Main; end C;"), 1);
}

// Take-before-first-advance: GetOwningEnumerator → TakeCurrent (no MoveNext) → IOE.
BEZEL_TEST(owning_enumerator_1349, take_before_advance_raises_ioe_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var caught := 0; "
        "    try "
        "      using var x := en.TakeCurrent(); "  // no MoveNext yet → IOE
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "  // 1 = IOE was raised
        "  end Main; end C;"), 1);
}

// Take-after-exhaustion: MoveNext → TakeCurrent → MoveNext (→ false) → TakeCurrent → IOE.
// Exhaustion is reached by advancing past the last element (MoveNext returns false);
// the subsequent TakeCurrent must raise IOE because current_val is null (§29.1 v0.161).
// Uses explicit sequential calls (no while loop) to reach state=-1.
// NOTE: no Bezel // comments in the source string — they would eat the rest of the
// (newline-free) concatenated literal.
BEZEL_TEST(owning_enumerator_1349, take_after_exhaustion_raises_ioe_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var b1 := en.MoveNext(); "
        "    using var x := en.TakeCurrent(); "
        "    var b2 := en.MoveNext(); "
        "    if b2 then return -2; end; "
        "    var caught := 0; "
        "    try "
        "      using var y := en.TakeCurrent(); "
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "
        "  end Main; end C;"), 1);
}

// Diagnostic: check each step for take_after_exhaustion independently.
// If MoveNext #2 returns false → returned 1 (correct). If true → 0 (wrong).
BEZEL_TEST(owning_enumerator_1349, diag_movenext_returns_false_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var b1 := en.MoveNext(); "  // → true
        "    using var x := en.TakeCurrent(); "  // consumes Box(1)
        "    var b2 := en.MoveNext(); "  // → false (exhaustion)
        "    if b2 then "
        "      return 0; "   // b2 was true — wrong
        "    else "
        "      return 1; "   // b2 was false — correct
        "    end; "
        "  end Main; end C;"), 1);
}

// Diagnostic: check that TakeCurrent on exhausted state raises IOE.
// Check b2 first (should be false), then try TakeCurrent.
BEZEL_TEST(owning_enumerator_1349, diag_take_after_exhaust_ioe_via_b2_spec_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var b1 := en.MoveNext(); "
        "    using var x := en.TakeCurrent(); "
        "    var b2 := en.MoveNext(); "
        "    if b2 then return -2; end; "  // b2 must be false
        "    var caught := 0; "
        "    try "
        "      using var y := en.TakeCurrent(); "
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "
        "  end Main; end C;"), 1);
}

// ===========================================================================
// §29.2 / #1349: for-loop desugaring still works (regression guard)
// ===========================================================================

// for-loop over IOwningEnumerable<T> implicitly calls TakeCurrent; elements
// are still received correctly and destructors run exactly once.
BEZEL_TEST(owning_enumerator_1349, for_loop_desugaring_correct_spec_29_2) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(); "
        "    yield return new R(); "
        "    yield return new R(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    for var item in Gen() do "
        "      using var u := move item; "
        "    end; "
        "    return R._n; "  // 3 = each destructor ran once
        "  end Main; end C;"), 3);
}

// ===========================================================================
// §29.1 / #1349: ASan — manual protocol: exactly-once disposal, no leak
// ===========================================================================

// AOT + ASan: manual protocol using source/MoveNext/TakeCurrent, 3 elements.
// Source held via using var so §29.8 owned params (none here) are disposed and
// the source frame is freed. Each element must be disposed exactly once.
BEZEL_TEST(owning_enumerator_1349, asan_manual_protocol_no_leak_spec_29_1) {
    int rc = runUnderASan(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(); "
        "    yield return new R(); "
        "    yield return new R(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      using var x := en.TakeCurrent(); "
        "    end; "
        "    return 0; "
        "  end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak, no double-free)
}

// ===========================================================================
// §28.11 / §28.3 / §28.7 family: builtin owned-TRANSFER results held in a
// plain `var` local (merge-gate ASan follow-up to #1349). unwindOwnedLocalClass
// missed calls with no MethodSymbol (builtin Pop/Dequeue/PopLast/.../
// TakeCurrent), so an explicit `x.Dispose()` dispatched the destructor but
// never RECLAIMED the block, and §28.7 drop-on-overwrite skipped the local.
// Now shares the using-var allowlist (isBuiltinOwnedTransferCall).
// ===========================================================================

// AOT + ASan: `var x := en.TakeCurrent(); ...; x.Dispose()` — the manual form
// the §29.1 desugaring itself uses (spec_0226) — reclaims exactly once.
BEZEL_TEST(owning_enumerator_1349, asan_plain_var_explicit_dispose_reclaims) {
    int rc = runUnderASan(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(); "
        "    yield return new R(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      var x := en.TakeCurrent(); "
        "      x.Dispose(); "
        "    end; "
        "    return 0; "
        "  end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak, no double-free)
}

// AOT + ASan: the same gap for the owning-collection transfers — §4.11
// OwningStack.Pop() into a plain `var` with explicit Dispose, plus a §28.7
// reassignment (`x := s.Pop()`) that must dispose the displaced element.
BEZEL_TEST(owning_enumerator_1349, asan_pop_plain_var_dispose_and_overwrite) {
    int rc = runUnderASan(
        "module M; " + kCountR +
        "class C "
        "  public static method Main(): int "
        "    using var s := new OwningStack<R>(); "
        "    s.Push(move (new R())); "
        "    s.Push(move (new R())); "
        "    var x := s.Pop(); "
        "    x := s.Pop(); "      // §28.7: displaced element disposed
        "    x.Dispose(); "       // §28.11: reclaim + null
        "    return 0; "
        "  end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak, no double-free)
}
