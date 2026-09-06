// Codegen — `emitDisposerCore` clobbers the ENCLOSING body's §27.5
// capture-cell reclaim state when it is reached MID-EMISSION (§27.5 / §28.4 /
// §28.5 / §28.9, v0.294; issue #3921, the disposal site of #3804's family).
//
// Spec §27.5 "Closures": "A lambda may reference variables from its enclosing
// scope. Captured variables are heap-allocated in a closure object."
//
// Spec §28.9 "Runtime-managed types": "Runtime management governs the
// *function value itself* — the heap-allocated closure object that backs it".
// The closure object, its env block and every capture cell are reclaimed when
// the closure's last reference is dropped; this test is about the compiler
// silently disarming that reclaim.
//
// Spec §28.5 "Generic class fields": "When a generic class declares
// `private owned _slot: T` (where T is a type parameter), the restriction
// 'only fields of class or interface types may be `owned`' is checked at
// *instantiation*." That per-instantiation check is what makes `new
// Box<Res>(…)` build a PER-INSTANTIATION specialized disposer (#816 / §28.5),
// which is the emitter re-entry this test is about.
//
// THE BUG. `emitDisposerCore` (LlvmBackend_Disposal.cpp) re-establishes the
// whole §27.5 capture-cell / closure-release eligibility family for the
// destructor body it is about to emit — `closureReleaseEligible_`,
// `boxCaptureLocals_`, `singleCaptureLocals_`, `definitelyEscapingLambdas_`,
// `cellFreeEligible_`, `unwindUnsafeCellFree_`, `sharedCellEnvdtorFreeOwner_`
// and their fn tokens (fix #2887) — by CLEARING each one and re-running its
// analysis. It never had the other half of that contract: a save/restore, so
// the state is left as it was found.
//
// That is harmless when disposers are emitted from the top-level disposer
// pass. It is NOT harmless on the two paths that call it MID-EMISSION of an
// ordinary body:
//
//   * `emitSpecializedDisposer`, reached from `specializedVtableFor` ->
//     `emitInstanceInit`, i.e. from `lowerNew` for a generic instantiation
//     with an `owned T` field (§28.5 above);
//   * `specializedDisposerCoreFor`, reached from `emitBaseDisposeChain`.
//
// Both save and restore the per-function emit state (`currentFn_`, `locals_`,
// the dispose/loop/finally stacks, the insert point) precisely because they
// run mid-emission — but neither ever grew a save for the capture-cell
// family. So a SINGLE `new Box<Res>(…)` anywhere in a method body wiped that
// body's own capture-cell eligibility for every lambda lowered AFTER it, with
// no diagnostic and no crash. A leak, never a double-free, and invisible to
// any test that does not instantiate an owned-`T` generic in the same body
// that also captures.
//
// THE FIX. `CaptureReclaimSetScope` (LlvmBackend_impl.hpp, added by fix #3500
// for exactly this asymmetry and already constructed by `emitIteratorMethod`,
// `emitAsyncMethod` and `emitAsyncIteratorMethod`) at the top of
// `emitDisposerCore` — reused as-is, no new scope type.
//
// MEASURED, at 500 iterations under ASan/LSan, on the pre-fix compiler:
// 26000 B / 1500 allocations for BOTH rows 1 and 2 — 52 B per activation:
// the closure header (40), its env (8) and the capture cell (4). The
// identical programs with the one `new Box<Res>(…)` line removed (row 3) are
// clean on BOTH compilers, which is what isolates the trigger to the
// mid-emission disposer re-entry rather than to the capture shape itself.
//
// LEAK AMPLIFICATION (see the project's one-shot-LSan note): a single
// iteration cannot distinguish "freed" from "still stack-reachable at exit",
// so every leak row loops 500 times. Each row also asserts the program's own
// captured stdout, not just the sanitizer verdict — "did not leak" cannot
// distinguish a correct run from one that exited early or short-circuited the
// amplifier, so without the value the rows could regress into vacuous passes.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

// expectCleanAndDidTheWork() — AOT with AddressSanitizer/LeakSanitizer on the
// GENERATED program (runAotChecked sets opts.sanitizeAddress). A leak, UAF or
// double-free makes the run abort with a non-zero status; a clean run reports
// 0. Plain `runAot` cannot see any of the three and is never used in this
// file.
//
// It asserts a VALUE as well as the sanitizer verdict, deliberately: "did not
// leak" alone cannot tell a correct run from a program that exited early or
// short-circuited its loop, so a future regression that broke the amplifier
// would silently turn these rows into vacuous passes. Every amplified row
// below runs `Run()` 500 times for a `Run()` that answers 3, so the captured
// stdout must be exactly "1500\n" — the amplification actually happened AND
// each activation computed the right captured value.
void expectCleanAndDidTheWork(const std::string& src,
                              const std::string& expectedStdout) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);      // false when ASan/LSan aborts the program
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The owned-`T`-field generic whose instantiation forces a per-instantiation
// specialized disposer (§28.5 "Generic class fields", #816) — the emitter
// re-entry under test. `Res` is the class argument (an owned `T` field admits
// only class/interface types).
const char* kOwnedGenericPrelude =
    "module M; import System; "
    "class Res "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Box<T> "
    "  private owned _value: T; "
    "  public constructor(owned v: T) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; ";

// The one line that triggers the mid-emission `emitDisposerCore` re-entry.
// Wrapped in `do … end` so the `using var` discharges immediately, well
// before the lambda below is lowered — the leak is about the COMPILER's
// state at lambda-lowering time, not about this object's own lifetime.
const char* kInstantiateOwnedGeneric =
    "    do "
    "      using var b := new Box<Res>(new Res()); "
    "    end; ";

const char* kAmplifierMain =
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..500 do "
    "      total := total + C.Run(); "
    "    end; "
    "    Console.WriteLine(total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ============================================================================
// Row 1 (LEAK-AMPLIFIED x500): the PLAIN, common shape — a captured local and
// one ordinary non-escaping `var` closure local, with the owned-`T` generic
// instantiated EARLIER IN THE SAME BODY. The instantiation's specialized
// disposer is emitted mid-`Run`, wiping `closureReleaseEligible_` /
// `cellFreeEligible_` / `boxCaptureLocals_` and their fn tokens, so `g`'s
// scope-end `bz_closure_release` and `x`'s capture-cell free were both
// silently never registered.
//
// Pre-fix: 26000 B / 1500 allocations. Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_capture_reclaim_clobber_3921,
           plain_capture_after_owned_generic_instantiation_leak_amplified_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(): int "
        "    var x := 3; " +
        kInstantiateOwnedGeneric +
        "    var g := lambda() => x; "
        "    return g(); "
        "  end Run; " +
        kAmplifierMain;
    expectCleanAndDidTheWork(src, "1500\n");
}

// ============================================================================
// Row 2 (LEAK-AMPLIFIED x500): fix #3804's own deeply-nested ESCAPING-lambda
// probe with the same one line inserted. This is the shape issue #3921 was
// filed about: the ancestor body proves `sharedCellEnvdtorFreeOwner_[x] ==
// inner` (§3426 conditions (a)-(e)), and the mid-emission disposer clear
// discards that decision before `outer`'s own `lowerLambda` can snapshot it
// for the #3804 merge — so `inner`'s envdtor never learns it is the
// designated sole owner of `x`'s shared cell.
//
// Pre-fix: 26000 B / 1500 allocations, identical to row 1 and to #3804's own
// measured figure. Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_capture_reclaim_clobber_3921,
           escaping_nested_lambda_after_owned_generic_leak_amplified_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 3; " +
        kInstantiateOwnedGeneric +
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); "
        "  end Make; "
        "  public static method Run(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Run; " +
        kAmplifierMain;
    expectCleanAndDidTheWork(src, "1500\n");
}

// ============================================================================
// Row 3 (CONTROL, x500): row 1 with the `new Box<Res>(…)` line REMOVED. Clean
// on BOTH compilers. This is what makes rows 1 and 2 a statement about the
// mid-emission disposer re-entry rather than about the capture shape: without
// this row a future regression in the ordinary capture path would be
// misattributed to this fix's subject.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_capture_reclaim_clobber_3921,
           control_no_owned_generic_instantiation_leak_amplified_500x) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var x := 3; "
        "    var g := lambda() => x; "
        "    return g(); "
        "  end Run; " +
        std::string(kAmplifierMain);
    expectCleanAndDidTheWork(src, "1500\n");
}

// ============================================================================
// Row 4 (SOUNDNESS, both directions): the destructor's OWN capture must still
// work. `CaptureReclaimSetScope` restores the CALLER's sets on the way out, so
// this row pins that it does not disturb what `emitDisposerCore` establishes
// for the destructor body itself while that body is being emitted — the
// destructor's own `var g := lambda() => y;` must still be released at its
// scope end AND must still read the correct captured value.
//
// The value check is the load-bearing half here, as it is for every row above
// (all of which assert their own stdout): a "no leak" verdict alone would also
// be satisfied by a destructor whose lambda never ran. `Res` prints its
// captured `n` on every disposal, so 200 disposals of `new Res(7)` produce
// exactly 200 lines of "7" — an observable that a wrongly-restored set would
// break. Runs under the same ASan/LSan net as the rows above, so a
// double-free introduced by restoring the sets would abort here rather than
// pass.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_capture_reclaim_clobber_3921,
           destructor_own_capture_still_reclaims_and_reads_correct_value) {
    std::string src =
        "module M; import System; "
        "class Res "
        "  public n: int; "
        "  public constructor(v: int) n := v; end constructor; "
        "  destructor "
        "    var y := this.n; "
        "    var g := lambda() => y; "
        "    Console.WriteLine(g().ToString()); "
        "  end destructor; "
        "end Res; "
        "class Box<T> "
        "  private owned _value: T; "
        "  public constructor(owned v: T) _value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Run(): int "
        "    var x := 3; "
        "    do "
        "      using var b := new Box<Res>(new Res(7)); "
        "    end; "
        "    var g := lambda() => x; "
        "    return g(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..200 do "
        "      total := total + C.Run(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan would have aborted
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 disposals, each printing the destructor lambda's captured `7`.
    int sevens = 0;
    for (std::size_t i = 0; i + 1 < r.stdoutText.size(); ++i)
        if (r.stdoutText[i] == '7' && r.stdoutText[i + 1] == '\n') ++sevens;
    BEZEL_EXPECT_EQ(sevens, 200);
}
