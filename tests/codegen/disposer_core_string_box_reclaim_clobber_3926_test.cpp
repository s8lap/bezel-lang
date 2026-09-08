// Codegen — `emitDisposerCore` clobbers the ENCLOSING body's §28.3 STRING-
// release and §28.9 BOX-release eligibility sets when it is reached
// MID-EMISSION (§27.5 / §28.3 / §28.4 / §28.5 / §28.9, v0.294; issue #3926,
// the disposal site of #2807 / #2854's family).
//
// Spec §28.3 "Owner/local binding":
//   "When a `var` local is initialized with a `+1` producer (string concat,
//    string interpolation, ToString, etc.), the compiler retains at bind and
//    emits a scope-end release."
//
// Spec §28.9 "Runtime-managed types":
//   "Every `+1` producer must be matched by exactly one disposer.  For
//    Object/interface-typed locals that wrap a value-typed source, the
//    disposer is `bz_box_release`."
//
// Spec §28.5 "Generic class fields":
//   "When a generic class declares `private owned _slot: T` (where T is a
//    type parameter), the restriction 'only fields of class or interface types
//    may be `owned`' is checked at *instantiation*." That per-instantiation
//    check is what makes `new Box<Res>(…)` build a PER-INSTANTIATION
//    specialized disposer (#816 / §28.5), which is the emitter re-entry this
//    test is about.
//
// THE BUG. `emitDisposerCore` (LlvmBackend_Disposal.cpp) re-establishes THREE
// independent reclaim families for the destructor body it is about to emit:
//   (a) the STRING-release family — `stringReleaseEligible_`,
//       `stringBorrowRetainEligible_`, `stringBorrowAll_`,
//       `stringLocalCandidates_`, `valueLikeChoiceStrReleaseEligible_`, and
//       `stringReleaseEligibleFn_` (fix #2807 / #2817 / #2854) — by CLEARING
//       each one and re-running `collectStringReleaseEligible`. NO SAVE/RESTORE.
//   (b) the BOX-release family — `boxReleaseEligible_`,
//       `boxReassignedLocals_`, `boxReleaseEligibleFn_` — by CLEARING the two
//       sets and re-running `collectBoxReleaseEligible` (and the token is
//       deliberately not re-pinned because the sets are AST-keyed; the issue
//       still wants the OUTER token preserved on return). NO SAVE/RESTORE.
//   (c) the capture-cell family — the existing `CaptureReclaimSetScope` from
//       fix #3921 covers this one.
//
// (a) and (b) are harmless when disposers are emitted from the top-level
// disposer pass (the next disposer re-establishes everything before it is
// needed).  They are NOT harmless on the two paths that call `emitDisposerCore`
// MID-EMISSION of an ordinary body:
//   * `emitSpecializedDisposer` — `specializedVtableFor` -> `emitInstanceInit`
//     -> `lowerNew` for a generic instantiation with an `owned T` field
//     (#816 / §28.5);
//   * `specializedDisposerCoreFor` — `emitBaseDisposeChain`.
// Both save and restore the per-function emit state (`currentFn_`, `locals_`,
// the dispose/loop/finally stacks, the insert point) precisely because they
// run mid-emission — but neither ever grew a save for the STRING or BOX
// families.  So a SINGLE `new Box<Res>(…)` anywhere in a method body silently
// disarmed that body's own §28.3 / §28.9 reclaims for every string / boxed
// local lowered AFTER it.  No diagnostic, no crash: a leak, never a
// double-free (the retain and the release are disarmed together, so the
// pairing stays balanced — exactly why nothing catches it).
//
// THE FIX.  Mirror `CaptureReclaimSetScope` (header-local RAII): hoist the
// hand-rolled six-member STRING save/restore that `buildSpecializedMethodFn`
// already performs into a `StringReclaimSetScope` RAII, and add a small
// three-member `BoxReclaimSetScope` twin.  Use both at the top of
// `emitDisposerCore`, immediately after the existing `CaptureReclaimSetScope`,
// so the outer body's eligibility is preserved on return from the disposer
// re-entry.
//
// MEASURED (issue #3926, at 500 iterations under ASan/LSan):
//   STRING repro: 19392 B / 1000 allocations for 500 Run() activations;
//                  38.78 B per activation — the 24-byte bz_string header plus
//                  the per-iteration concat payload (n.ToString for n>9).  The
//                  no-`Box<Res>` control is clean.
//   BOX repro:    12000 B / 500 allocations; 24 B per activation — the box
//                  itself.  The no-`Box<Res>` control is clean.
//
// LEAK AMPLIFICATION (see the project's one-shot-LSan note): a single
// iteration cannot distinguish "freed" from "still stack-reachable at exit",
// so every leak row loops 500 times.  Each row also asserts the program's own
// captured stdout, not just the sanitizer verdict — "did not leak" cannot
// distinguish a correct run from one that exited early or short-circuited
// the amplifier, so without the value the rows could regress into vacuous
// passes.

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
// would silently turn these rows into vacuous passes.
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
// before the string/box local below is lowered — the leak is about the
// COMPILER's state at the lowering site of the next local, not about this
// object's own lifetime.
const char* kInstantiateOwnedGeneric =
    "    do "
    "      using var b := new Box<Res>(new Res()); "
    "    end; ";

const char* kAmplifierMain =
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..500 do "
    "      total := total + C.Run(i); "
    "    end; "
    "    Console.WriteLine(total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ============================================================================
// Row 1 (LEAK-AMPLIFIED x500): the STRING-release half.  The owned-`T`
// generic is instantiated EARLIER IN THE SAME BODY, so the disposer re-entry
// clears `stringReleaseEligible_` (and the five sibling string sets + token)
// for the OUTER `C.Run`.  The next local — `var s := "abc" + n.ToString();` —
// is then lowered with the eligibility set emptied: its bind-time retain and
// scope-end release both go UNARMED, so the bz_string header (and concat
// payload) leaks per activation.
//
// Pre-fix: 19392 B / 1000 allocations; 38.78 B per activation.  Post-fix:
// clean.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_string_box_reclaim_clobber_3926,
           string_local_after_owned_generic_instantiation_leak_amplified_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(n: int): int "
        "    var ignored := 0; " +
        kInstantiateOwnedGeneric +
        "    var s := \"abc\" + n.ToString(); "
        "    var len := s.Length; "
        "    ignored := ignored + len; "
        "    return ignored; "
        "  end Run; " +
        kAmplifierMain;
    // Total length sum for n=1..500:
    //   n in 1..9:   9 iters * ("abc"+1-char.ToString).Length == 9*4 == 36
    //   n in 10..99: 90 iters * 5 == 450
    //   n in 100..499: 400 iters * 6 == 2400
    //   n == 500:    1 iter * 6 == 6
    //   total: 2892
    expectCleanAndDidTheWork(src, "2892\n");
}

// ============================================================================
// Row 2 (CONTROL, x500): row 1 with the `new Box<Res>(…)` line REMOVED.
// Clean on BOTH compilers.  This is what makes row 1 a statement about the
// mid-emission disposer re-entry rather than about the string concat shape:
// without this row a future regression in the ordinary string-local path
// would be misattributed to this fix's subject.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_string_box_reclaim_clobber_3926,
           control_string_local_no_owned_generic_instantiation_clean_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(n: int): int "
        "    var ignored := 0; "
        "    var s := \"abc\" + n.ToString(); "
        "    var len := s.Length; "
        "    ignored := ignored + len; "
        "    return ignored; "
        "  end Run; " +
        kAmplifierMain;
    expectCleanAndDidTheWork(src, "2892\n");
}

// ============================================================================
// Row 3 (LEAK-AMPLIFIED x500): the BOX-release half.  Same prelude, the
// post-`do` local is `var o: Object := n;` (an int→Object box), so the
// mid-emission disposer re-entry clears `boxReleaseEligible_` for the OUTER
// `C.Run` and `o`'s scope-end `bz_box_release` goes UNARMED.  `is int`
// (`box_refcount_test.cpp`'s pattern) replaces the `o <> nil` guard — Bezel
// has no `<>` operator on `Object`; the BOX itself is never null, so the
// type-test is true every iteration.
//
// Pre-fix: 12000 B / 500 allocations; 24 B per activation — the box itself.
// Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_string_box_reclaim_clobber_3926,
           box_local_after_owned_generic_instantiation_leak_amplified_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(n: int): int "
        "    var ignored := 0; " +
        kInstantiateOwnedGeneric +
        "    var o: Object := n; "
        "    if o is int then ignored := ignored + 1; end; "
        "    return ignored; "
        "  end Run; " +
        kAmplifierMain;
    expectCleanAndDidTheWork(src, "500\n");
}

// ============================================================================
// Row 4 (CONTROL, x500): row 3 with the `new Box<Res>(…)` line REMOVED.
// Clean on BOTH compilers.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_string_box_reclaim_clobber_3926,
           control_box_local_no_owned_generic_instantiation_clean_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(n: int): int "
        "    var ignored := 0; "
        "    var o: Object := n; "
        "    if o is int then ignored := ignored + 1; end; "
        "    return ignored; "
        "  end Run; " +
        kAmplifierMain;
    expectCleanAndDidTheWork(src, "500\n");
}

// ============================================================================
// Row 5 (LEAK-AMPLIFIED x500): COMBINED — both a string local AND a boxed
// local follow the disposer re-entry in the same body.  This pins that the
// two new scopes coexist cleanly (no member-list overlap with the existing
// `CaptureReclaimSetScope`, no interaction at restore time).
//
// Pre-fix: both leaks add up.  Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_disposer_core_string_box_reclaim_clobber_3926,
           string_and_box_locals_after_owned_generic_leak_amplified_500x) {
    std::string src = std::string(kOwnedGenericPrelude) +
        "class C "
        "  public static method Run(n: int): int "
        "    var ignored := 0; " +
        kInstantiateOwnedGeneric +
        "    var s := \"abc\" + n.ToString(); "
        "    var o: Object := n; "
        "    if o is int then ignored := ignored + s.Length; end; "
        "    return ignored; "
        "  end Run; " +
        kAmplifierMain;
    // Same 2892 as row 1 (the boxed int is always an int, so the guard
    // always fires and `s.Length` is added each iteration).
    expectCleanAndDidTheWork(src, "2892\n");
}
