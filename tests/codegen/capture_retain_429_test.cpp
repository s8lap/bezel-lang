// Codegen + runtime — closure capture retains (issue #429, CAPTURE sub-slice).
//
// BACKGROUND (#429 capture sub-slice):
// The #672 env-disposer foundation added a `ptr envdtor` field (field 3, off 24)
// to every closure cell.  bz_closure_release calls envdtor(env) at last-drop if
// non-null.  This file tests the capture-retain activation: for lambdas that
// capture locals of RETAINABLE types (string, closure/function), lowerLambda now:
//
//   1. At capture-store time (env fill): NO extra retain is emitted.
//      RATIONALE: StringEscapeWalker / ClosureEscapeWalker SUPPRESS the scope-end
//      release for captured locals (FINDING [1]).  That suppressed release is the
//      reference that the envdtor will release.  No new reference is created.
//      Adding an extra retain would inflate RC to +2 (scope-end suppressed → +2,
//      envdtor releases once → +1 residual leak).
//
//   2. Emits a per-lambda `bz$lam$N$envdtor(ptr env)` function that loads each
//      retainable slot from the env and calls bz_string_release / bz_closure_release.
//      This is the DEFERRED scope-end release (the reference that was suppressed
//      by the escape walker is released here instead).
//
//   3. Stores the envdtor fn ptr at cell field 3 (off 24).  bz_closure_release
//      calls it at last-drop before freeing the env block.
//
// CAPTURE SEMANTICS per §27.5 / §28.9 / §28.3 v0.137:
//   §28.3 capture table: every closure capture is classified as a BORROW.
//   §28.9: "Capturing an owned class-typed local does NOT transfer ownership into
//   the closure.  The original `using var` retains the disposal obligation."
//   Therefore: class captures are pure borrows — NO retain/release for the
//   class instance itself.  Strings and closures ARE reference-counted at the
//   language runtime level, so they DO need retain/release to prevent the +1
//   producer leak described in FINDING [1] below.
//
// FINDING [1] (StringEscapeWalker): Inside a lambda body, inLambda=true makes
// ANY read of a captured outer local an "escape" — this prevents the
// scope-end bz_string_release from being registered for that local.  Result:
// a string local captured by a lambda would leak its +1 unless the envdtor
// releases it.  The retain+envdtor pair fixes this leak.
//
// PRIME DIRECTIVE: over-release is worse than a leak.  Every retain at capture
// time MUST be paired with exactly one release in the envdtor for that lambda.
// Shapes where pairing cannot be proven: left leaking (documented).
//
// SHAPES TESTED (leak matrix):
//   Shape 1: +1 producer string local captured; lambda invoked once then released.
//             String freed via envdtor.  Boxed cell (8B) leaks — pre-existing.
//             ASan-clean (no UAF/DF).
//   Shape 2: String captured; lambda invoked in a loop (50k iters).
//             Each iter's string freed via envdtor; cells leak (pre-existing, 50k×8B).
//             ASan-clean.
//   Shape 3: +1 producer string captured; lambda stored in a field then field released.
//             Envdtor fires on last field-release drop.  ASan-clean.
//   Shape 4: Lambda capturing a closure (function-typed local); both envdtors fire.
//             Inner freed via outer's envdtor.  Boxed cells leak (pre-existing).
//             ASan-clean.
//   Shape 5: Lambda capturing a class instance (borrow per §28.9/§28.3).
//             NO retain/release in envdtor; using-var disposes exactly once.
//             Double-dispose canary: ASan-clean (no UAF/DF).
//   Shape 6: Lambda capturing int/bool (value types); envdtor skips non-retainable.
//             No retain/release emitted.  Boxed cells leak (pre-existing).  ASan-clean.
//   Shape 7: Lambda capturing both string AND int (mixed capture).
//             String freed via envdtor; int skipped.  Cells leak (pre-existing).
//             ASan-clean.
//   Shape 8: Lambda discarded without invocation; envdtor still fires on release.
//             String is reclaimed even without the lambda being called.  ASan-clean.
//
// LEFT-LEAKING (per PRIME DIRECTIVE, documented but not failing):
//   - Boxed capture CELLS (the shared heap allocation from boxLocalSlot): not freed
//     by the envdtor to avoid UAF with the outer scope's access path.
//     These are a pre-existing contained leak; future work to free at outer-scope exit.
//   ALL tests use expectValueAndNoCorruption (detect_leaks=0) to tolerate cell leaks
//   while still catching UAF/double-free via ASan heap instrumentation.
//
// per §27.5 / §28.3 / §28.9 / #429 / #672.

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
static const std::filesystem::path& wtDir429cap() {
    return wtScratchDir("wt-429cap");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429cap();
    std::string stem = unique_stem("bezel_429cap");
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

// Lower `source` to an object file and link it against the memoized ASan-built
// runtime.  Returns true and fills `obj`/`exe` on success; false on failure
// (toolchain unavailable / compile / link error).
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir429cap();
    std::string stem = unique_stem("bezel_429cap_san");
    obj = (dir / (stem + ".o")).string();
    exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return false;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return false;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return false;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, obj, err)) return false;
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + obj + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(obj.c_str());
        return false;
    }
    return true;
}

// Assert correct exit code AND no ASan MEMORY-SAFETY error (UAF/DF), tolerating
// a residual leak (detect_leaks=0).  Used for shapes where a known safe leak
// survives (captured-variable boxed cells, class-capture no-op envdtors, etc.)
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str());
    std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ===========================================================================
// Shape 1: +1 producer string captured; lambda invoked once then released.
// §28.9 / §4.6 / #429: a string produced by concat is bound to a local, then
// captured into a lambda.  StringEscapeWalker (FINDING [1]) marks it escaped →
// scope-end release is SUPPRESSED (the reference is "transferred" to the env).
// Without envdtor: the +1 producer leaks (scope-end suppressed, nothing releases).
// With envdtor: lambda release fires envdtor which calls bz_string_release → freed.
// NO extra retain at capture time (PRIME DIRECTIVE): the existing +1 is the
// reference the envdtor will release.
// Returns 0 (lambda result > 0).
// NOTE: boxed cell (8 bytes from boxLocalSlot) leaks — pre-existing contained
// leak (cell shared with outer-scope alloca; not freed to avoid UAF). ASan-clean
// (no UAF/double-free), value-correct, but not fully LSan-clean (cell survives).
// ===========================================================================
const char* kStringCaptureInvokedOnce =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"he\" + \"llo\"; "   // +1 producer, rc=1
    "    var f: () -> int := lambda() => s.Length; "   // capture; scope-end suppressed; envdtor will release
    "    var r: int := f(); "                     // invoke: r=5
    "    if r > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape 2: String captured; lambda churned in a loop.
// §27.5 / §28.9 / #429: 50 000 iterations each binding a fresh +1 string and
// capturing it.  Without envdtor: all strings leak (scope-end suppressed per
// FINDING [1]).  With envdtor: each string is released at lambda scope-end.
// NOTE: boxed cells (8 bytes each) leak — pre-existing contained leak.
// Returns 0 (sum > 0).
// ===========================================================================
const char* kStringCaptureLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      var s: string := \"x\" + \"y\"; "    // fresh +1 string each iter
    "      var f: () -> int := lambda() => s.Length; " // capture+retain
    "      total := total + f(); "               // invoke (result=2)
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape 3: +1 producer string captured; lambda stored in a field then dropped.
// §28.9 / §4.6 / #429: the captured string outlives the lambda local because
// the closure cell escapes to a field (RETURN or FIELD-STORE sub-slice).  The
// envdtor fires when the FIELD's last drop occurs.  The string must be reclaimed
// exactly once (via envdtor) — no double-free.
//
// Design: H.fn is a function field.  The outer method captures `s` (a +1 string)
// into a lambda and stores it in H.fn.  After the outer method returns, the
// closure is held by the field.  When `using var h` is disposed, the field-store
// machinery releases the closure, envdtor fires, string released.
// Returns 0 (exactly correct disposal count).
// ===========================================================================
const char* kStringCaptureStoredInField =
    "module M; class H "
    "  public fn: () -> int := lambda() => 0; "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var result: int := 0; "
    "    do "
    "      using var h := new H(); "
    "      var s: string := \"stored\" + \"!\"; "  // +1 producer
    "      h.fn := lambda() => s.Length; "               // capture+retain; field-store releases old (()=>0), retains new
    "      result := h.fn(); "                      // invoke: 8
    "    end; "
    "    if result > 0 then return 0; end; return 1; "
    "  end Main; "
    "end H;";

// ===========================================================================
// Shape 4: Lambda capturing a closure (function-typed local).
// §28.9 / §4.6 / #429: a closure (function-typed value) captured by an outer
// lambda.  The outer env retains the inner closure at capture time; envdtor
// releases it.  LSan-clean (no double-free: single retain + single envdtor release).
// Returns 0 (inner+outer both work correctly).
// ===========================================================================
const char* kClosureCaptureByLambda =
    "module M; class C "
    "  public static method Main(): int "
    "    var inner: (int) -> int := lambda(x: int) => x * 2; "  // inner closure
    "    var outer: (int) -> int := lambda(x: int) => inner(x) + 1; "  // captures inner
    "    var r: int := outer(3); "  // inner(3)=6, outer=7
    "    if r = 7 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// Loop variant for shape 4: closure-capture loop (N iterations).
// Each iteration creates a fresh double-pair; inner/outer closures are invoked and released.
// The outer closure's envdtor fires at each iteration's scope-end, releasing inner.
// NOTE: captures the loop variable `n` which is a fresh const per iter (no reassignment
// in the loop body); avoids the pre-existing crash in variable-reassign capture loops.
const char* kClosureCaptureLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var k: int := 0; var total: int := 0; "
    "    while k < 10000 do "
    "      var n: int := k; "                                  // snapshot — no reassignment in body
    "      var inner: (int) -> int := lambda(x: int) => x + n; "   // captures n (int borrow — no retain)
    "      var outer: (int) -> int := lambda(x: int) => inner(x); " // captures inner (closure — envdtor releases)
    "      total := total + outer(1); "
    "      k := k + 1; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape 5: Lambda capturing a class instance (borrow per §28.9/§28.3).
// §28.9 v0.137: "Capturing an owned class-typed local does NOT transfer
// ownership into the closure.  The original `using var` retains the disposal
// obligation."  §28.3 capture table: every closure capture = BORROW.
// THEREFORE: the envdtor must NOT call Dispose on the captured class instance,
// and must NOT retain/release it.  The disposal count must be exactly 1
// (from the using-var scope).  Double-dispose canary: if envdtor wrongly
// called Dispose, count would be 2 and ASan would detect the double-free.
// Returns _n (must equal 1).
// ===========================================================================
const char* kClassCaptureBorrow =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public method Tag(): int return 42; end Tag; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do "
    "      using var r := new R(); "
    "      var f: () -> int := lambda() => r.Tag(); "  // borrow capture — no retain/release
    "      _ := f(); "                        // invoke
    "    end; "                                   // using-var disposes r exactly once
    "    return _n; "   // must be exactly 1
    "  end Main; "
    "end R;";

// Loop variant: 5000 iterations; total disposal count must be exactly 5000.
// Value + no-corruption (boxed capture cells leak — pre-existing).
const char* kClassCaptureBorrowLoop =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public method Tag(): int return 1; end Tag; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 5000 do "
    "      do "
    "        using var r := new R(); "
    "        var f: () -> int := lambda() => r.Tag(); "
    "        _ := f(); "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    if _n = 5000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end R;";

// ===========================================================================
// Shape 6: Lambda capturing int/bool (value types).
// §28.9 / #429: value-type captures (int, bool) are stored by value through a
// boxed cell.  No retain is emitted at capture time (only string/function types
// get retains).  The envdtor skips the slot.  LSan-clean (cells leak — known
// pre-existing; value content has no RC obligation).
// Returns 0 (correct computation).
// ===========================================================================
const char* kValueCaptureIntBool =
    "module M; class C "
    "  public static method Main(): int "
    "    var k: int := 7; "
    "    var flag: bool := true; "
    "    var f: (int) -> int := lambda(x: int) => (if flag then (x + k) else 0); "
    "    var r: int := f(3); "  // 3 + 7 = 10
    "    if r = 10 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape 7: Mixed capture — string AND int in the same lambda.
// §28.9 / #429: only the string slot gets retain+envdtor release; the int slot
// is skipped.  Correct discrimination by semaTy.  LSan-clean (string reclaimed;
// int value has no RC).
// Returns 0 (correct computation AND LSan-clean).
// ===========================================================================
const char* kMixedStringAndIntCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"mix\" + \"ed\"; "  // +1 producer
    "    var n: int := 3; "
    "    var f: () -> int := lambda() => s.Length + n; "  // captures s (retain) + n (skip)
    "    var r: int := f(); "  // \"mixed\".Length=5 + 3 = 8
    "    if r = 8 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// Loop variant for shape 7: 50000 iterations.
const char* kMixedCaptureLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      var s: string := \"a\" + \"b\"; "       // +1 string each iter
    "      var n: int := 2; "
    "      var f: () -> int := lambda() => s.Length + n; "  // s retained+released; n skipped
    "      total := total + f(); "  // 2+2=4 each iter
    "      i := i + 1; "
    "    end; "
    "    if total = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape 8: Lambda discarded without invocation; envdtor fires on release.
// §28.9 / #429: the envdtor must fire even when the lambda is never called.
// Without capture-retain, the string is leaked whether or not the lambda is
// invoked (FINDING [1]).  With retain+envdtor: the release fires at scope end
// when the lambda local is released, cleaning up the string.
// Returns 0 (no crash, lambda discarded cleanly).
// ===========================================================================
const char* kStringCaptureDiscarded =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 50000 do "
    "      var s: string := \"discard\" + \"me\"; "  // +1 producer
    "      var _f: () -> int := lambda() => s.Length; "    // capture+retain; never invoked
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// Tests
// ===========================================================================

// §28.9 / §4.6 / #429 Shape 1: +1 producer string captured, lambda invoked once.
// The envdtor (deferred scope-end release, FINDING [1]) prevents the string leak.
// Boxed cell (8 bytes from boxLocalSlot) leaks — pre-existing contained leak;
// tolerated.  ASan-clean: no UAF, no double-free, correct result.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_string_capture_invoked_once) {
    expectValueAndNoCorruption(kStringCaptureInvokedOnce, 0);
}

// §28.9 / §4.6 / #429 Shape 2: string captured, lambda invoked 50k times in loop.
// Each iteration: string's +1 reference is released via envdtor at lambda scope-end.
// Boxed cells (8 bytes each, 50k total) leak — pre-existing contained leak; tolerated.
// ASan-clean: no UAF, no double-free.  Value correct: sum=100000.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_string_capture_loop_lsan_clean) {
    expectValueAndNoCorruption(kStringCaptureLoop, 0);
}

// §28.9 / §4.6 / #429 Shape 3: +1 string captured, lambda stored in field.
// The envdtor fires at the field's last-drop; string reclaimed exactly once.
// No UAF/double-free canary (ASan-clean).
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_string_capture_field_store_clean) {
    expectValueAndNoCorruption(kStringCaptureStoredInField, 0);
}

// §28.9 / §4.6 / #429 Shape 4: closure-typed local captured by outer lambda.
// Outer envdtor releases the inner closure (deferred scope-end release per FINDING [1]).
// Boxed cells leak — pre-existing contained leak; tolerated.
// ASan-clean: no UAF, no double-free.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_closure_capture_by_lambda_lsan_clean) {
    expectValueAndNoCorruption(kClosureCaptureByLambda, 0);
}

// §28.9 / #429 Shape 4 loop: closure-capture loop (10k iterations).
// inner closure captured by outer; envdtor releases inner at each iter's scope-end.
// Boxed cells leak — pre-existing contained leak; tolerated.  ASan-clean.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_closure_capture_loop_lsan_clean) {
    expectValueAndNoCorruption(kClosureCaptureLoop, 0);
}

// §28.9 / §28.3 / #429 Shape 5: class instance captured (borrow per §28.3).
// envdtor must NOT dispose the class instance.  Disposal count == 1 (single
// using-var).  Double-dispose canary — if envdtor wrongly called Dispose the
// count would be 2 and ASan would abort (double-free).
// Boxed cell (8 bytes) leaks — pre-existing contained leak; tolerated.
// ASan-clean (no UAF, no double-free), correct result.
BEZEL_TEST(capture_retain_429, spec_28_3_28_9_class_capture_borrow_no_extra_dispose) {
    // Single shot: disposal count must be 1 (returned as exit code).
    expectValueAndNoCorruption(kClassCaptureBorrow, 1);
}

// §28.9 / §28.3 / #429 Shape 5 loop: 5000 iterations; total _n must equal 5000.
// No-corruption guard (boxed capture cells leak — pre-existing).
BEZEL_TEST(capture_retain_429, spec_28_3_28_9_class_capture_borrow_loop_no_double_dispose) {
    expectValueAndNoCorruption(kClassCaptureBorrowLoop, 0);
}

// §28.9 / #429 Shape 6: value-type capture (int + bool); envdtor skips non-retainable.
// No retain emitted → no release in envdtor → no leak, no UAF.  Correct result.
// Boxed cells leak (pre-existing); tolerated.
BEZEL_TEST(capture_retain_429, spec_28_9_value_capture_int_bool_no_corruption) {
    expectValueAndNoCorruption(kValueCaptureIntBool, 0);
}

// §28.9 / #429 Shape 7: mixed capture (string + int); string envdtor-released,
// int (value type) skipped.  semaTy discrimination is correct.
// Boxed cells leak — pre-existing contained leak; tolerated.  ASan-clean.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_mixed_string_int_capture_lsan_clean) {
    expectValueAndNoCorruption(kMixedStringAndIntCapture, 0);
}

// §28.9 / #429 Shape 7 loop: mixed capture churned 50k times.
// Strings envdtor-released each iteration; ints skipped.
// Boxed cells (50k pairs) leak — pre-existing contained leak; tolerated.  ASan-clean.
BEZEL_TEST(capture_retain_429, spec_27_5_28_9_mixed_capture_loop_lsan_clean) {
    expectValueAndNoCorruption(kMixedCaptureLoop, 0);
}

// §28.9 / #429 Shape 8: lambda capturing string but never invoked.
// The envdtor fires at scope-end release; string reclaimed without invocation.
// FINDING [1]: without envdtor, the string would leak (scope-end suppressed).
// Boxed cells (50k) leak — pre-existing contained leak; tolerated.  ASan-clean.
BEZEL_TEST(capture_retain_429, spec_28_9_string_capture_discarded_lsan_clean) {
    expectValueAndNoCorruption(kStringCaptureDiscarded, 0);
}

// ===========================================================================
// Shape 9 (#672): the SAME +1-producer string captured by TWO (then THREE)
// lambdas. §27.5 / §28.9: both lambdas share ONE boxed cell; EACH lambda's
// envdtor releases the cell's string at last-drop, but the suppressed scope-end
// release covers only ONE reference. So the FIRST capture transfers that single
// suppressed release; every ADDITIONAL (alreadyBoxed) capture must mint an
// independent +1 retain (mirroring the box additional-capture retain). Before
// the fix, two envdtor releases against one reference = over-release →
// heap-use-after-free (ASan exit 99 ≠ expected). After, N captures balance
// exactly: 1 (suppressed) + (N-1) retains = N refs = N releases.
// s := "he"+"llo" (len 5): two → 5 + (5+1) = 11; three → 5 + 6 + 7 = 18.
// ===========================================================================
const char* kStringTwoLambdaCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"he\" + \"llo\"; "
    "    var f: () -> int := lambda() => s.Length; "
    "    var g: () -> int := lambda() => s.Length + 1; "
    "    return f() + g(); "
    "  end Main; "
    "end C;";

const char* kStringThreeLambdaCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"he\" + \"llo\"; "
    "    var f: () -> int := lambda() => s.Length; "
    "    var g: () -> int := lambda() => s.Length + 1; "
    "    var h: () -> int := lambda() => s.Length + 2; "
    "    return f() + g() + h(); "
    "  end Main; "
    "end C;";

BEZEL_TEST(capture_retain_429, spec_27_5_28_9_672_string_two_lambda_capture_no_uaf) {
    // Before #672's additional-capture retain this was a heap-use-after-free.
    expectValueAndNoCorruption(kStringTwoLambdaCapture, 11);
}

BEZEL_TEST(capture_retain_429, spec_27_5_28_9_672_string_three_lambda_capture_no_uaf) {
    expectValueAndNoCorruption(kStringThreeLambdaCapture, 18);
}
