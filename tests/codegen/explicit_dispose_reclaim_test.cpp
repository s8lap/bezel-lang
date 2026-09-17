// Codegen — explicit `.Dispose()` must reclaim the object's heap block
// (issue #415, §28.2 / §28.4 / §28.12).
//
// After #411 split DISPATCH from RECLAMATION (Dispose became idempotent via a
// per-object run-once flag, §28.12 line 5624: "The runtime tracks completion at
// the granularity of the object, not per-method"), emitDynamicDispose only
// DISPATCHES the idempotent Dispose; the OWNING auto-dispose site reclaims the
// block via emitDisposeAndReclaim. The EXPLICIT `.Dispose()` call lowering was
// left dispatch-only, so an owned value whose ONLY disposal is an explicit
// `.Dispose()` (a plain `var` owned local disposed via `x.Dispose()` with no
// auto-dispose follow; an `owned` parameter explicitly disposed) ran its
// destructor but LEAKED its heap block — the free was skipped.
//
// The fix: at the explicit `.Dispose()` site, after dispatching the (idempotent)
// Dispose, reclaim the block (emitDisposeAndReclaim) AND null the receiver
// slot/field, mirroring the auto-dispose site. §28.2: "the first call performs
// the disposal; subsequent calls are no-ops" — the run-once flag makes a second
// DISPATCH a no-op, but a second RECLAIM (free) would double-free, so only the
// first disposal reclaims and the slot is nulled so a following auto-dispose
// sees null and skips.
//
// Two complementary checks (mirroring free_on_dispose_test #380 and
// exception_disposal_test #383/#411):
//
//   1. Destructor-count tests (native exe, always run): the destructor bumps a
//      static counter; the exit code observes how many times it ran. Proves the
//      destructor ran EXACTLY once across the explicit + any auto dispose site
//      (count==1; a double-run would be 2, a crash would not return).
//
//   2. LeakSanitizer legs (native, only when an ASan toolchain is present):
//      assert the program is ASan/LSan-clean — no leak (the #415 fix reclaims
//      the block) and no double-free / use-after-free (the slot-null guard makes
//      a following auto-dispose a no-op). Verified load-bearing: case (a) leaks
//      its block before the fix; case (b) must stay double-free-safe.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir415() {
    return wtScratchDir("wt-415");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 if compilation/link/run failed. Mirrors
// free_on_dispose_test.cpp::compileAndRun.
int compileAndRun(const std::string& source) {
    auto dir = wtDir415();
    std::string stem = unique_stem("bezel_415");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}


// LeakSanitizer verification for a single program. Lowers `source` to an object
// file, links it against an ASan-instrumented build of the runtime, and runs the
// binary under LeakSanitizer. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir415();
    std::string stem = unique_stem("bezel_415_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    // Step 1: front-end + lower the program to a native object file.
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    // Step 2: compile the runtime with ASan, then link both into an ASan binary.
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }

    // Step 3: run under LeakSanitizer.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Run both checks: assert the destructor-count exit code, and (if the toolchain
// is present) assert LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;          // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);        // 0 == LSan clean (no leak, no UAF/DF)
}

// --- Programs (exit code encodes destructor activity; LSan checks the free) ---

// (a) §28.2 / §28.4: a plain `var` owned local whose ONLY disposal is an explicit
// `.Dispose()`. The destructor runs once (exit 1); before the #415 fix the heap
// block leaked (the explicit-dispose lowering dispatched but never reclaimed).
// This is the #415 baseline reproducer — load-bearing: LSan reports the leak
// before the fix, clean after.
const char* kExplicitLocalOnly =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var r := new R(); "
    "    r.Dispose(); "
    "    return _n; "
    "  end Main; "
    "end R;";

// (a2) §28.2: a REPEATED explicit `x.Dispose()` on the same owned local must be a
// no-op — idempotent disposal is guaranteed ("a further Dispose() call remains a
// no-op, not an error, in both modes"). The destructor must run EXACTLY once
// (exit 1), and the second dispose must NOT re-dispatch the vtable Dispose
// through the now-nulled slot (a use-after-free / null-deref SIGSEGV). This is
// the case the old "double-dispose is a compile error" rule masked; with that
// downgraded to a warning (§28.2), the second call must be a runtime no-op.
// Load-bearing: must stay ASan/LSan-clean (no UAF, no double-free, no leak).
const char* kDoubleExplicitDispose =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var r := new R(); "
    "    r.Dispose(); "
    "    r.Dispose(); "
    "    return _n; "
    "  end Main; "
    "end R;";

// (b) §28.2 / §28.12: a `using var` explicitly disposed BEFORE its scope-end
// auto-dispose. The destructor must run EXACTLY once (exit 1) and the block must
// be freed EXACTLY once — the explicit dispose reclaims and nulls the slot, so
// the scope-end auto-dispose sees null and skips. NO double-free. Load-bearing:
// this is the case that must stay ASan-clean (a naive "always reclaim at the
// explicit site" without the slot-null would double-free here).
const char* kUsingThenExplicit =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do "
    "      using var d := new R(); "
    "      d.Dispose(); "
    "    end; "
    "    return _n; "
    "  end Main; "
    "end R;";

// (c) regression guard, §28.2 / §28.4: an object disposed ONLY by the scope-end
// auto-dispose (no explicit `.Dispose()`). The destructor runs once (exit 1) and
// the block is reclaimed once — the #411/#380 baseline must stay correct.
const char* kAutoOnly =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var d := new R(); end; "
    "    return _n; "
    "  end Main; "
    "end R;";

// (d) §28.4 base chain: a plain `var` owned local of a DERIVED class disposed
// only via an explicit `.Dispose()`. The most-derived Dispose runs the derived
// destructor (+1) then chains to the base (+100) = 101, and both blocks (one
// object, full chain) are reclaimed exactly once at the explicit site.
const char* kExplicitLocalBaseChain =
    "module M; class Base "
    "  public static _n: int := 0; "
    "  destructor Base._n := Base._n + 100; end destructor; "
    "  public static method Main(): int "
    "    var d := new Derived(); "
    "    d.Dispose(); "
    "    return Base._n; "
    "  end Main; "
    "end Base; "
    "class Derived : Base "
    "  destructor Base._n := Base._n + 1; end destructor; "
    "end Derived;";

// (e) #1018 / §25 / §28.5: a plain `var` owned local bound from an owned-returning
// binary OPERATOR result (`var r := a + b`), explicitly disposed. The destructor
// runs once for each of a, b, r (live-count returns to 0). Before #1018 the
// operator-result BLOCK leaked: explicitDisposeOwnedReceiverSlot did not classify
// the BinaryExpr-initialized local as reclaimable (unwindOwnedLocalClass only knew
// `new`/owned-call), so `r.Dispose()` dispatched the destructor but never reclaimed
// the block. After #1018 the operator result is recognized like a `new`/owned-call
// result: the explicit dispose reclaims + nulls the slot. Load-bearing on the LSan
// leg (the destructor-count is 0 with or without the fix; only LSan sees the leak).
const char* kOperatorResultExplicitDispose =
    "module M; class Cell "
    "  public static _n: int := 0; "
    "  public Value: int; "
    "  public constructor(v: int) _n := _n + 1; Value := v; end constructor; "
    "  destructor _n := _n - 1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.Value + b.Value); "
    "  public static method Main(): int "
    "    do "
    "      using var a := new Cell(1); "
    "      using var b := new Cell(2); "
    "      var r := a + b; "
    "      r.Dispose(); "
    "    end; "
    "    return _n; "
    "  end Main; "
    "end Cell;";

// (f) #965 / §28.3 regression guard: the SAME owned operator result WITHOUT an
// explicit dispose must still auto-drop at scope end (the affine auto-drop). The
// #1018 unwind-classifier change must not perturb this path — it stays disposed
// exactly once (live-count 0, LSan clean).
const char* kOperatorResultAutoDrop =
    "module M; class Cell "
    "  public static _n: int := 0; "
    "  public Value: int; "
    "  public constructor(v: int) _n := _n + 1; Value := v; end constructor; "
    "  destructor _n := _n - 1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.Value + b.Value); "
    "  public static method Main(): int "
    "    do "
    "      using var a := new Cell(1); "
    "      using var b := new Cell(2); "
    "      var r := a + b; "
    "    end; "
    "    return _n; "
    "  end Main; "
    "end Cell;";

// (g) #1024 / §28.3 / §28.5: an INTERFACE-typed local initialized with a concrete
// `new Factory()` and disposed via an explicit `f.Dispose()`. classOfLocal returns
// the declared INTERFACE symbol (kind != Class), so unwindOwnedLocalClass's `new`
// case (a) missed the concrete class and explicitDisposeOwnedReceiverSlot stayed
// dispatch-only: the destructor ran once via the vtable (exit 1 with or without the
// fix) but the 16-byte block LEAKED — only the LSan leg catches it. The concrete-
// typed equivalent (`var f := new Factory(); f.Dispose()`) reclaims, so this was an
// interface/concrete parity gap. The fix recovers the concrete class from the
// NewExpr. Load-bearing on LSan (destructor-count is 1 either way).
const char* kInterfaceLocalExplicitDispose =
    "module M; "
    "interface IThing method Ping(): int; end IThing; "
    "class Factory : IThing "
    "  public static _n: int := 0; "
    "  public method Ping(): int => 9; "
    "  destructor Factory._n := Factory._n + 1; end destructor; "
    "end Factory; "
    "class C public static method Main(): int "
    "  var f: IThing := new Factory(); "
    "  var p := f.Ping(); "
    "  f.Dispose(); "
    "  return Factory._n; "
    "end Main; end C;";

// (h) #1024 / §28.3: the same interface-typed local explicitly disposed, then an
// exception unwinds the scope. The fix newly registers the unwind-only disposal for
// an interface-typed `new` local (registerUnwindOwnedLocal via unwindOwnedLocalClass),
// so this guards that the explicit dispose's slot-null makes the unwind disposal a
// no-op — destructor runs EXACTLY once (exit 1) and NO double-free. Load-bearing on
// LSan (a missing null-guard would double-free the block on the unwind edge).
const char* kInterfaceExplicitThenThrow =
    "module M; "
    "interface IThing method Ping(): int; end IThing; "
    "class Factory : IThing "
    "  public static _n: int := 0; "
    "  public method Ping(): int => 9; "
    "  destructor Factory._n := Factory._n + 1; end destructor; "
    "end Factory; "
    "class C "
    "  public static method Boom(): int throw new ArgumentException(\"x\"); end Boom; "
    "  public static method Main(): int "
    "    try "
    "      var f: IThing := new Factory(); "
    "      f.Dispose(); "
    "      var z := C.Boom(); "
    "      return z; "
    "    catch "
    "      var e: Exception => return Factory._n; "
    "    end; "
    "  end Main; "
    "end C;";

}  // namespace

BEZEL_TEST(codegen_explicit_dispose_reclaim_415, plain_local_explicit_only_freed) {
    // §28.2/§28.4: destructor runs once; block reclaimed at the explicit site
    // (before #415 it leaked). LSan: 0 leaked.
    expectDisposedAndLeakFree(kExplicitLocalOnly, 1);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_415, using_then_explicit_no_double_free) {
    // §28.2/§28.12: explicit dispose THEN scope-end auto-dispose — destructor
    // once, free once, slot nulled so the auto-dispose skips. NO double-free.
    expectDisposedAndLeakFree(kUsingThenExplicit, 1);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_415, auto_only_still_freed_once) {
    // Regression: an object disposed only by the scope-end auto-dispose still
    // frees exactly once (#380/#411 baseline unaffected).
    expectDisposedAndLeakFree(kAutoOnly, 1);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_415, explicit_local_base_chain_freed) {
    // §28.4: explicit dispose of a derived local runs Derived(+1)+Base(+100)=101
    // and reclaims the (single) block once after the full base chain.
    expectDisposedAndLeakFree(kExplicitLocalBaseChain, 101);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_1018, operator_result_explicit_dispose_freed) {
    // #1018/§25/§28.5: explicit dispose of an owned binary-operator result reclaims
    // the block (before the fix it leaked — only the LSan leg catches it). All three
    // (a, b, r) destruct exactly once → live-count 0; LSan: 0 leaked, no double-free.
    expectDisposedAndLeakFree(kOperatorResultExplicitDispose, 0);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_1018, operator_result_auto_drop_freed) {
    // #965 regression: the owned operator result without an explicit dispose still
    // auto-drops at scope end (live-count 0, LSan clean) — the #1018 unwind-class
    // change does not perturb the affine auto-drop path.
    expectDisposedAndLeakFree(kOperatorResultAutoDrop, 0);
}

// §28.2: a repeated explicit `x.Dispose(); x.Dispose();` is a no-op on the second
// call — destructor runs exactly once (exit 1), and no use-after-free /
// double-free (the second dispatch is null-guarded). Regression for the
// Error→Warning downgrade of double-dispose.
BEZEL_TEST(codegen_explicit_dispose_reclaim_415, double_explicit_dispose_runs_once_no_uaf_28_2) {
    expectDisposedAndLeakFree(kDoubleExplicitDispose, 1);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_1024, interface_typed_local_explicit_dispose_freed) {
    // #1024/§28.3/§28.5: explicit dispose of an INTERFACE-typed local initialized
    // with a concrete `new Factory()` reclaims the block (before the fix it leaked
    // — only the LSan leg catches it; the destructor-count is 1 either way). The
    // concrete-typed equivalent already reclaimed; this closes the parity gap.
    expectDisposedAndLeakFree(kInterfaceLocalExplicitDispose, 1);
}

BEZEL_TEST(codegen_explicit_dispose_reclaim_1024, interface_typed_explicit_then_throw_no_double_free) {
    // #1024/§28.3: the interface-typed local's explicit dispose nulls the slot, so
    // the newly-registered unwind disposal finds null and skips on the throw edge —
    // destructor runs exactly once (exit 1), no double-free, LSan clean.
    expectDisposedAndLeakFree(kInterfaceExplicitThenThrow, 1);
}
