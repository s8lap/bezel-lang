// Codegen / refcounting — issues #5280 and #5281, the two follow-ups the
// non-author review of #5262 filed, measured on the staging tip r23-c28
// 28644650 and pinned as test rows for the shapes that are CLEAN.
//
// #5262's borrowed-string-argument pin is a CALLER-side retain released on the
// call's normal-return edge (`releaseBorrowedStringArgPins`). Two edges that
// edge does not describe reached it. MEASURED here, 200 iterations per probe,
// `BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh --probe --asan`:
//
//   #5280  ASYNC callee                 0 leaks / 0 crashes, 200/200
//   #5281  THROWING callee, method call  200/200 LEAKS, 0 crashes — the
//                                        PRODUCER argument, a pre-existing #429
//   #5281  BOUND METHOD-REFERENCE call   200/200 LEAKS, 0 crashes on the issue's
//                                        own repro; a DIFFERENT shape of the
//                                        same call form is a use-after-free.
//                                        NOT PINNED, see the tail of this file.
//
// ── WHAT IS PINNED HERE, AND WHAT IS NOT ───────────────────────────────────
//
//   #5280 shape 1  ASYNC callee, `await c.Twice(c.Val, "q"+i)`, the issue's
//                  own repro verbatim. CLEAN on 28644650, fixed by 1c3f8277.
//                  ROWS 1a (the repro) + 1b (its no-suspension control).
//
//   #5281 shape 1  THROWING callee, method-call arm. The orphaning shape is
//                  ALREADY PINNED by
//                  `method_call_arm_orphan_pin_5262_test.cpp:162`, so this
//                  file adds only ROW 2a, that arm's REACHABILITY CONTROL —
//                  the one the sibling file does not have.
//
//   #5281 shape 2  BOUND METHOD-REFERENCE call. NOT PINNED. The issue's own
//                  repro is the same #429 producer leak as shape 1; the
//                  shape that IS a use-after-free is the orphaning-store
//                  variant its repro does not reach. A row would freeze
//                  either an unrelated defect or a memory-safety one.
//                  NO ROW, deliberately. Reported to the coordinator.
//
//   #5281 producer-argument leak  NOT PINNED. See "WHY #5281's ISSUE REPRO IS
//                  NOT A ROW" below — that leak is a pre-existing #429
//                  producer-release issue, and a row here would freeze it.
//
// ── WHY #5281's ISSUE REPRO IS NOT A ROW, AND WHAT IS INSTEAD ──────────────
//
// #5281's repro is `c.Twice(c.Val, "q" + i.ToString())` in a THROWING callee.
// Measured on 28644650 that repro STILL leaks, and the leak is the SECOND
// argument: a runtime-produced `string` whose own release is
// normal-return-only, so it tracks the PRODUCER count and not the pin. Four
// discriminating probes separate the two (MEASURED by the #5262 fix-forward on
// d68dab26, and re-measured on this tip for the method-ref half):
//
//   callee body                       argument         result
//   ------------------------------    ---------------  ---------------------
//   stores over field, then THROWS    c.Val (borrow)   LEAKS  <- the pin
//   stores over field, then THROWS    "q"+i (producer)  LEAKS  (#429 producer)
//   no field store, then THROWS       c.Val (borrow)   CLEAN  <- row (2a)
//   no field store, then THROWS       "q"+i (producer)  LEAKS  (same total)
//
// Both arms leak on the orphaning shape, so the totals ALONE do not attribute
// the leak to the pin. What attributes it is the ORPHANING store: the
// orphaning row displaces the receiver field with a FRESH runtime string
// (`x + "x"`, never a literal) so the pin's +1 is the only reference keeping
// the borrowed value allocated. A path that merely released LATE would still
// release before the process exits; a path that never releases leaks the pin's
// +1 even though the producer argument is separately accounted for.
//
// LeakSanitizer reports UNREACHABLE blocks, and a string the receiver field
// still points at is REACHABLE, which MASKS a dropped +1. That is exactly the
// property row (2a) asserts, and it is why every probe here builds its strings
// at RUNTIME (a literal is an rc sentinel that hides the bug entirely).
//
// ── THE ORACLE, AND WHY IT IS `runAotChecked` ──────────────────────────────
//
// `runAotChecked` (tests/support/aot_support.hpp:3722) is a self-contained
// leak oracle: it compiles with `opts.sanitizeAddress = true` and runs the
// program with `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0`,
// then returns `ran = false` on ANY sanitizer report — leak, UAF, or double
// free (aot_support.hpp:3690-3714). It needs no environment variable. The
// `BEZEL_AOT_DETECT_LEAKS` knob gates `runAot` (aot_support.hpp:3286), NOT
// `runAotChecked`, whose leak detection is unconditional.
//
// Each row drives BOTH halves:
//   plain `runAot`      — program logic, no sanitizer. A non-leak crash that
//                         still printed the right lines would sail past a
//                         checked leg alone; the plain leg is what nails the
//                         program-logic half.
//   `runAotChecked`     — the memory net. `ran` false on any report, and the
//                         exit code and stdout re-assert the program on the
//                         instrumented binary.
//
// `ran == false` is asserted FIRST in every row, and the row returns on it:
// a false `ran` means a sanitizer report, not "the program misbehaved", and
// asserting the stdout after it would read a `--did-not-run` sentinel.
//
// ── THE ITERATION COUNT ────────────────────────────────────────────────────
//
// 200 for every row, matching the review's own measurement, so a row's verdict
// and the coordinator's probe count are the same experiment. The 5262 rows
// that overlap these shapes use 50 for the async arm; the count does not
// weaken either verdict (a dropped +1 or a freed borrow is a FIRST-iteration
// failure, and LeakSanitizer is not a sampler), but 200 is what the brief
// measured, so 200 is what is asserted.
//
// Spec: §4.6 "`string`" and §28.9 "Runtime-managed types" — the
// reference-counting implementation is unspecified, so the contract is only
// that a parameter naming a `string` value keeps reading that value for the
// whole call, and a suspension inside the callee is part of that call.
// §28.3 "Local variables, ownership, and borrows" for the exception-path
// discharge a throwing edge has to honour. §30.1 "`async` methods", and within
// it the rule "Borrowed parameters, `this`, and captures are held until
// settlement", for row group 1: an async method captures its parameters into a
// heap state-machine frame that outlives the synchronous call, so the pin has
// to survive the first suspension. Spec revision v0.313.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ── ROW GROUP 1 — #5280, the ASYNC callee ──────────────────────────────────
//
// #5280's repro, verbatim: an `async` callee that stores over the aliased
// field twice, SUSPENDS, and only then reads the borrowed parameter. The
// caller-side pin releases at the call's normal-return edge, which for an
// `async` method is its FIRST SUSPENSION (§30.1: the method "suspends and
// returns its `Task` to the caller"), so the +1 is gone long before the read.
//
// `async Main` awaits each task in turn, so the suspensions are sequential and
// stdout is deterministic. TaskScope/Spawn would make the order
// nondeterministic and the exact-stdout assertion a flake.
std::string asyncSrc(int iters, bool suspends) {
    return "module M5280; "
           "public class C "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public async method Twice(x: string, y: string): Task<int> "
           "    _v := x; "
           "    _v := y; "
           + std::string(suspends ? "await Task.Delay(0); " : "") +
           "    Console.WriteLine(x); "
           "    return 0; "
           "  end Twice; "
           "  public property Val: string => _v; "
           "end C; "
           "public class Program "
           "  public static async method Main(): Task<int> "
           "    using var c := new C(\"a1\"); "
           "    for var i in 0..<" + std::to_string(iters) +
           " do "
           "      await c.Twice(c.Val, \"q\" + i.ToString()); "
           "    end for; "
           "    Console.WriteLine(c.Val); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The value the parameter names on iteration i: "a1" for the first, then the
// PREVIOUS iteration's "q" string (the callee's second store put it there).
// The trailing line is the field after the loop, i.e. the last "q" string.
std::string asyncWant(int iters) {
    std::string w = "a1\n";
    for (int i = 0; i < iters - 1; ++i) w += "q" + std::to_string(i) + "\n";
    w += "q" + std::to_string(iters - 1) + "\n";
    return w;
}

// ── ROW GROUP 2 — #5281, the THROWING callee, method-call arm ─────────────
//
// The REACHABILITY form, and the only one this file builds: the callee reads
// the borrowed parameter and throws WITHOUT displacing the field, so `_v`
// still points at the value and a dropped +1 stays invisible to
// LeakSanitizer.
//
// The contrasting ORPHANING form — the callee writes the borrow into `_v` and
// then displaces `_v` with a FRESH runtime string (`x + "x"`), leaving the
// pin's +1 as the only reference — is what makes a throwing-edge leak
// observable, and it is already pinned by
// `method_call_arm_orphan_pin_5262_test.cpp:162`. This row is the control that
// file does not have.
std::string throwingMethodSrc(int iters) {
    return "module M5281; "
           "public class C "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public method Boom1(x: string) "
           "    Console.WriteLine(x); "
           "    throw new InvalidOperationException(\"boom\"); "
           "  end Boom1; "
           "  public property Val: string => _v; "
           "end C; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new C(\"a1\"); "
           "    var caught := 0; "
           "    for var i in 0..<" + std::to_string(iters) +
           " do "
           "      try "
           "        c.Boom1(c.Val); "
           "      catch "
           "        InvalidOperationException => do caught := caught + 1; end, "
           "      end try; "
           "    end for; "
           "    Console.WriteLine($\"caught={caught}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #5280 — THE ASYNC CALLEE
// ═══════════════════════════════════════════════════════════════════════════

// (1a) THE ISSUE'S REPRO, 200 iterations. The parameter read happens on the
// RESUME, in a different frame from the call, after the caller-side pin was
// released at the call's normal-return edge (here: the first suspension).
// Every `q<i>` line prints, which is the assertion that matters: a freed
// borrow prints garbage or SEGVs rather than the value.
BEZEL_TEST(borrowed_string_arg_pin_5280_5281,
           async_callee_param_survives_suspension_spec_4_6_28_9_v0_313) {
    const std::string src = asyncSrc(200, /*suspends=*/true);

    // plain leg: program logic, no sanitizer
    auto plain = runAot(src);
    BEZEL_EXPECT_EQ(plain.stdoutText, asyncWant(200));

    // checked leg: ASan + LeakSanitizer. `ran` false on any report.
    auto checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, asyncWant(200));
}

// (1b) CONTROL for (1a), and the row that makes (1a)'s failure attributable:
// the SAME class and the SAME double-store-then-read body with the `await`
// REMOVED, so the read happens before the call returns. The caller's
// normal-return pin is already live at that point, so this row is clean
// before AND after the fix — which is what says (1a)'s failure is the
// SUSPENSION and not the double store.
BEZEL_TEST(borrowed_string_arg_pin_5280_5281,
           control_async_callee_without_suspension_spec_4_6_28_9_v0_313) {
    const std::string src = asyncSrc(200, /*suspends=*/false);

    auto plain = runAot(src);
    BEZEL_EXPECT_EQ(plain.stdoutText, asyncWant(200));

    auto checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, asyncWant(200));
}

// ═══════════════════════════════════════════════════════════════════════════
// #5281 — THE THROWING CALLEE (method-call arm)
// ═══════════════════════════════════════════════════════════════════════════

// NO ROW FOR THE ORPHANING THROW. That shape — 200 throwing method calls of a
// borrow-only argument, callee stores the parameter into `_v` then displaces
// `_v` with a fresh runtime string (`x + "x"`) so the pin's +1 is the only
// reference, then throws — is ALREADY PINNED, row-for-row, by
// `method_call_arm_orphan_pin_5262_test.cpp:162`
// (`method_call_throwing_orphan_pin_does_not_leak_spec_28_3`), whose header
// documents the same `x + "x"` orphaning store, the same 200 iterations, and
// the same FAIL-WITHOUT against a91342d2. Re-adding it here would be a second
// row spending an AOT compile+link to assert a shape another row already holds
// with a proven revert. The one thing this file adds for #5281 is (2a) below:
// the arm's own reachability control, which that file does not have.
//
// (2a) The reachability CONTROL for the orphaning row above: the SAME throw and
// the SAME borrow-only argument, but the callee does NOT displace the field —
// it only reads the parameter and throws, so the value stays REACHABLE from
// `_v`. That is precisely the shape in which a dropped +1 is INVISIBLE to
// LeakSanitizer (it reports unreachable blocks, and a field still pointing at
// the value keeps it reachable), which is why the orphaning store is what makes
// the sibling row a real pin.
//
// This row's job is attribution, and it earns it in the FAIL-WITHOUT run: with
// a91342d2's method-call hunk reverted, the ORPHANING row in
// `method_call_arm_orphan_pin_5262_test.cpp` fails while this one stays green,
// and that pair is what says the leak is the orphaning store plus the unwind
// edge rather than the throw/catch machinery or the borrow itself. It is
// clean before and after the fix by construction — a control, not a pin.
BEZEL_TEST(borrowed_string_arg_pin_5280_5281,
           control_throwing_method_call_value_stays_reachable_spec_4_6_28_9_v0_313) {
    const std::string src = throwingMethodSrc(200);

    // The callee prints the borrowed parameter BEFORE it throws, so the
    // expected stdout is that line once per iteration, then the tally. It
    // never varies: the field is never displaced here, so every iteration
    // reads the SAME value, the one the constructor stored.
    std::string want;
    for (int i = 0; i < 200; ++i) want += "a1\n";
    want += "caught=200\n";

    auto plain = runAot(src);
    BEZEL_EXPECT_EQ(plain.stdoutText, want);

    auto checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, want);
}

// ═══════════════════════════════════════════════════════════════════════════
// #5281 SHAPE 2 — THE BOUND METHOD-REFERENCE CALL. NOT PINNED.
//
// #5281 filed this shape as a bounded leak. MEASURED on the staging tip
// 28644650, the issue's own repro `probe_method_ref.bzl` is indeed only a
// leak — 7290 B / 400 allocs on 200/200 iterations, 0 crashes — and it is
// byte-for-byte the SAME #429 producer leak the throwing callee shows (same
// total, every allocation attributed to `main`).
//
// The memory-safety defect is a DIFFERENT shape the issue's repro does not
// exercise. The discriminator is the callee's SECOND store:
//   `_v := y`         (the issue's repro — displaces with the other borrow,
//                     so the value stays referenced: leak only)
//   `_v := x + "x"`   (a FRESH string — orphans the pin, so the callee's own
//                     store frees what the parameter names: SEGV in
//                     bz_string_to_utf8z, src/runtime/bezel_runtime_
//                     strings.cpp:1250)
//
// NO ROW HERE EITHER WAY. The repro shape is a #429 producer leak, not this
// pin's, and pinning it would freeze an unrelated defect; the orphaning shape
// is a use-after-free, and a row would have to assert the crash, freezing a
// memory-safety defect into the suite. Both are reported to the coordinator
// instead — the numbers are in the lane handback and the draft comment, not
// restated here because they are a live measurement, not a pinned invariant.
// ═══════════════════════════════════════════════════════════════════════════
