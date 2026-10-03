// Codegen / refcounting — issue #5262, the follow-up to #5262-ff-rv's NOTE 1:
// the METHOD-CALL arm's pin is pinned by ZERO rows in
// `borrowed_string_arg_pin_5262_test.cpp`, even though that arm's
// `Calls2.cpp` install is live and load-bearing. Reverting ONLY that arm's
// hunk leaves all 15 existing rows green while the method-call orphan
// still leaks.
//
// WHY THIS ROW EXISTS. #5262's pin spans BOTH arms: the ctor arm's install
// lives in `lowerNew` (LlvmBackend_Exprs2.cpp) and the method-call arm's
// install lives in `lowerCallDispatch` (LlvmBackend_Calls2.cpp). The two
// releases on the normal-return edge are mirrored in `releaseBorrowedString
// ArgPins`, and the throw-edge releases are mirrored via
// `stringPinArgUnwindCleanup` as `unwindTarget_` across the method call
// (in `lowerCallDispatch`) and across the constructor's pending check (in
// `lowerNew`). #5262-ff's coverage set is 15 rows, all but two of which
// route the throwing callee through the ctor arm; the only method-call row
// (`control_in_module_twin_of_imported_row_spec_4_6_28_9`, shape 2b) is a
// normal-return control — it cannot pin a throw-edge leak.
//
// WHAT IS PINNED HERE. The method-call arm's install on the throw edge. The
// 200-iteration throwing shape — `c.Boom2(c.Val)` against a callee that
// stores the borrowed parameter BACK into the aliased field, then DISPLACES
// the field with a FRESH runtime string (`x + "x"`, not a literal sentinel),
// then throws — orphans the borrow. The pin's +1 is then the only reference
// keeping the original value alive, and a missing unwind-edge release leaks
// it. MEASURED, on a build with ONLY the Calls2.cpp hunk reverted:
//   without the hunk: `SUMMARY: AddressSanitizer: 26865 byte(s) leaked in
//     398 allocation(s)`, exit 1
//   with the hunk:    clean, exit 0
// (398, not 400: the first iteration's value is still reachable from the
// field when the process tears down.)
//
// WHY THE SECOND STORE IS `x + "x"` AND NOT A LITERAL — exactly the same
// vacuity trap the 5262-ff ctor row hit. LeakSanitizer reports UNREACHABLE
// blocks; a string the receiver field still points at is reachable, so a
// dropped +1 on it is masked. `x + "x"` is a FRESH runtime string, so
// displacing the field with it orphans the pinned value. The first version
// of this row stored a literal for the second value and passed with AND
// without the Calls2 install — a vacuous pin.
//
// Spec: §28.3 "Local variables, ownership, and borrows", K1.4 (Exception
// discharge applies to a by-borrow string parameter) and K7.4 (the throw
// edge runs the same cleanup chain as the normal-return edge, modulo the
// release). v0.313. The companion half is §4.6 "`string`" reference counting
// and §28.9 "Runtime-managed types" (the +1/pin); those are the ones the
// existing file cites, so this file does not duplicate them.
//
// THE ORACLE. runAotChecked — generated-code ASan + LeakSanitizer. The
// assertion is on stdout (`caught=200`) because a UAF can exit 0; the
// LEAK half is carried by the harness, so a missing unwind-edge release
// fails this row through the harness rather than through the compared
// string. Each row loops 200 iterations.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Run a Bezel source through `runAotChecked`. Returns the captured stdout
// for comparison, or a sentinel if the program did not execute. A LSan
// report (imbalance, leak) is carried by the harness and fails the row.
std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// SHAPE: method-call arm orphan. `BoomO.Boom2` stores the borrowed
// parameter into `_v` (so the field's sole reference is shared with the
// borrow pin's +1), then DISPLACES `_v` with a fresh runtime string
// (`x + "x"` — the orphaning store), then throws. The pin's +1 on the
// borrowed value is the only reference that keeps it allocated; without
// the throw-edge release it leaks. The caller catches and counts so stdout
// is deterministic.
std::string methodCallOrphanSrc(int iters) {
    return "module M5262note1; "
           "public class BoomO "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public method Boom2(x: string) "
           "    _v := x; "
           "    _v := x + \"x\"; "
           "    throw new InvalidOperationException(\"boom\"); "
           "  end Boom2; "
           "  public property Val: string => _v; "
           "end BoomO; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new BoomO(\"a1\"); "
           "    var caught := 0; "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      try "
           "        c.Boom2(c.Val); "
           "      catch "
           "        InvalidOperationException => do caught := caught + 1; end, "
           "      end try; "
           "    end; "
           "    Console.WriteLine($\"caught={caught}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// SHAPE: throw control without a borrowed arg. `BoomC.Boom` throws with no
// parameter at all — no pin to drop, no producer string, no field store.
// If the throw path itself leaked, this row would fail; its continued
// greenness is what makes Row 1's failure attributable to the borrowed
// argument rather than to the throw/call path. Note: this is a vacuous
// method-call throw (no parameter), not a ctor throw — it exercises the
// method-call arm's exception edge WITHOUT any string pin involvement.
std::string throwingControlSrc(int iters) {
    return "module M5262note1ctl; "
           "public class BoomC "
           "  public constructor() end constructor; "
           "  public method Boom() "
           "    throw new InvalidOperationException(\"boom\"); "
           "  end Boom; "
           "end BoomC; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new BoomC(); "
           "    var caught := 0; "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      try "
           "        c.Boom(); "
           "      catch "
           "        InvalidOperationException => do caught := caught + 1; end, "
           "      end try; "
           "    end; "
           "    Console.WriteLine($\"caught={caught}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// METHOD-CALL ARM — throwing callee with borrowed argument
// ═══════════════════════════════════════════════════════════════════════════

// (1) The reviewer probe (rv_methodcall_orphan.bzl) as a row. 200
// iterations of `c.Boom2(c.Val)`. Without the Calls2.cpp install: LSan
// reports 26865 B / 398 allocs and the row exits 1; with the install:
// clean and `caught=200`. The single argument pins, so the missing
// throw-edge release is the only explanation for the leak.
//
// Per-row non-vacuity: FOUR assertions — plain `runAot` stdout (the program
// logic: 200 throws caught), `runAotChecked` ran (compile + link + exec),
// `runAotChecked` exit code (no leak under ASan/LSan, set via
// `BEZEL_AOT_DETECT_LEAKS=strict`), and `runAotChecked` stdout (re-affirms
// the program logic on the ASan-instrumented binary). The plain leg also
// catches a regression where the program itself miscomputes (a non-leak
// crash would still print "caught<200\n" under the checked leg, so the
// plain leg is what nails the program-logic half).
BEZEL_TEST(method_call_arm_orphan_pin_5262,
           method_call_throwing_orphan_pin_does_not_leak_spec_28_3) {
    // plain: program logic, no sanitizer
    auto plain = runAot(methodCallOrphanSrc(200));
    BEZEL_EXPECT_EQ(plain.stdoutText, "caught=200\n");

    // checked: ASan/LSan — the leak oracle. Reads the same `runAotChecked`
    // exit code path the suite uses; BEZEL_AOT_DETECT_LEAKS=strict makes a
    // leak set r.ran=false (the harness's own chokepoint). With it OFF,
    // ASan is not linked, so the leak oracle is silent — this row's FAIL
    // signal rides on the strict env var.
    auto checked = runAotChecked(methodCallOrphanSrc(200));
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, "caught=200\n");
}

// (2) Throw control WITHOUT a borrowed parameter. Same try/catch shape,
// same throwing callee, but the callee takes no `string` parameter. If the
// method-call arm's throw path itself leaked, this row would fail —
// continued greenness makes Row 1's failure attributable to the borrow
// pin's missing unwind-edge release, not to the throw/call path.
//
// Per-row non-vacuity: same four-assertion shape as Row 1, on the no-arg
// variant. Plain stdout (`caught=200`) + checked ran + checked exit 0 +
// checked stdout (`caught=200`). The plain leg is what makes (2) a
// control for the program itself rather than for the leak oracle.
BEZEL_TEST(method_call_arm_orphan_pin_5262,
           control_throwing_method_call_without_borrowed_arg_spec_28_3) {
    auto plain = runAot(throwingControlSrc(200));
    BEZEL_EXPECT_EQ(plain.stdoutText, "caught=200\n");

    auto checked = runAotChecked(throwingControlSrc(200));
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, "caught=200\n");
}