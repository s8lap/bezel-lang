// Regression tests for #1097 (§31.5 / §31.2.1): a `throw` expression used as a
// catch-arm `when` guard crashed codegen — the throw's allocated object pointer
// was emitted as the i1 branch condition (`br ptr poison` → IR verification
// failure). A `throw` is a bottom-typed expression usable in any expression
// position (§31.5), and a guard that throws supersedes the in-flight exception
// (§31.2.1). The fix detects the guard diverged (threw) and skips the branch —
// the throw propagates via its own unwind branch. Found by adversarial
// discovery sweep #7.
//
// runAot observes Main()'s return via the process exit code (8-bit wrapped,
// aot_support.hpp); every value used here is ≤ 100, so it round-trips exactly.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* E2 =
    "module M; class E1 end E1; class E2 end E2; ";
}  // namespace

// #1097: a `throw` in a `when` guard compiles (was an IR-verification crash).
// Here the throwing-guard arm is never triggered (a different exception is
// thrown and caught by a later arm), proving the throw-in-guard lowers cleanly.
BEZEL_TEST(throw_in_catch_guard_1097, throw_guard_arm_not_triggered) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  try "
        "    throw new E2(); "
        "  catch E1 when throw new E1() => return 1; "    // guard never runs
        "  catch E2 => return 7; "                        // E2 caught here
        "  end; "
        "  return 0; "
        "end Main; end C;"), 7);
}

// §31.2.1: when the guard DOES throw, the new exception supersedes the in-flight
// one and propagates — here an outer try catches it (program runs to
// completion, proving the supersede semantics, not just no-crash).
BEZEL_TEST(throw_in_catch_guard_1097, guard_throw_supersedes_outer_catches) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  try "
        "    try "
        "      throw new E1(); "
        "    catch E1 when throw new E2() => return 1; "  // guard throws E2
        "    end; "
        "  catch E2 => return 9; "                        // outer catches E2
        "  end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// A NORMAL (non-throwing) guard that is true selects its arm.
BEZEL_TEST(throw_in_catch_guard_1097, normal_guard_true) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  var n := 5; "
        "  try throw new E1(); "
        "  catch E1 when n > 3 => return 1; "
        "  catch E1 => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A normal guard that is false falls through to the next arm.
BEZEL_TEST(throw_in_catch_guard_1097, normal_guard_false_falls_through) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  var n := 1; "
        "  try throw new E1(); "
        "  catch E1 when n > 3 => return 1; "
        "  catch E1 => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 2);
}

// ── #1108 / §31.2.1: a guard that throws frees the SUPERSEDED in-flight ───────
// exception exactly once. When a `when` guard throws (superseding the in-flight
// E1 with E2), the original E1 was previously freed ZERO times — a 16-byte leak
// (the arm's caught-exception disposal is registered only in the arm body, which
// is unreachable on the guard-throw edge). The fix exposes E1 as the caught
// object leaving the arm during guard lowering, so the guard's throw routes
// through emitThrowValue's existing #397/#866 caught-exception free (disposing
// E1 once, guarded against double-free by isDirectRethrow / throwLeavesArm).
// runAot exit codes don't observe the leak directly; leak-freedom is gated by
// the project's ASan sweep. These cases pin the CONTROL FLOW (correct supersede
// routing) and stress the free path so a double-free regression — which would
// crash the in-process JIT suite (the latent #1108 SIGSEGV class) — is caught.

// #1108: the superseding exception skips a non-matching outer arm and is caught
// by the matching one (E1 -> guard throws E2 -> outer `catch E2`), rc=9.
BEZEL_TEST(throw_in_catch_guard_1097, guard_throw_supersede_skips_nonmatching_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1 end E1; class E2 end E2; class E3 end E3; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new E1(); "
        "    catch E1 when throw new E2() => return 1; "
        "    catch E3 => return 2; "                     // not matched
        "    end; "
        "  catch E2 => return 9; "                       // superseding E2 caught here
        "  end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// #1108: the guard-throw supersede repeated 100x — each iteration's superseded
// E1 must be freed exactly once. A double-free regression crashes the in-process
// JIT run here; a leak would accumulate (caught by the ASan harness). rc=100.
BEZEL_TEST(throw_in_catch_guard_1097, guard_throw_supersede_repeated_no_corruption) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 100 do "
        "    try "
        "      try throw new E1(); "
        "      catch E1 when throw new E2() => total := total + 1000; "
        "      end; "
        "    catch E2 => total := total + 1; "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  return total; "                              // 100
        "end Main; end C;"), 100);
}

// ── #1150 / §31.2.1: a guard that diverges supersedes the in-flight exception ──
// and propagates — exactly like the #1097/#1108 fresh-throw guard cases above.
// The issue reported rc=1 (arm body runs) using a `catch E1 ex` binding form
// that does not parse; with the real `catch ex: E1` binding the #1108
// guard-divergence machinery already routes the supersede correctly. These pin
// the behavior the issue flagged as untested. (§31.2.1: a guard that throws is
// "not caught by a later arm of the same try and does not resume guard
// evaluation"; the superseding exception propagates to the ENCLOSING handler.)
//
// AOT-migration finding (JIT->AOT divergence, class B): the issue's own repro
// used `catch ex: E1 when throw ex => …` — a guard that names the BOUND catch
// variable as the throw operand. §31.1 is explicit that a `throw` operand is
// consumed as an OWNED value, so "a *borrowed* class reference is not a valid
// operand" — and a catch-arm binding is a borrow (§31.2.1) — so `throw ex;` is
// rejected at every position, guard or arm body ("to re-raise the exception
// being handled, use the bare re-raise", §31.2.4). The in-process JIT harness
// never ran the ownership pass, so it silently accepted the invalid `throw ex`
// and these two tests happened to pass anyway; the stricter AOT front end
// (this migration's whole point) correctly rejects it. Bare `throw` (§31.2.4's
// actual re-raise spelling) is in turn syntactically valid only inside a catch
// ARM BODY, not a guard (`catch ex: E1 when throw => …` is a parse error) — so
// there is no spec-valid way to re-raise-by-guard at all. Reshaped below to a
// FRESH same-type throw (`throw new E1()`, a valid owned operand per §31.1),
// which preserves the control-flow assertion these tests exist to pin (guard
// divergence supersedes and propagates to the enclosing handler, skipping a
// later same-try arm) without the invalid borrowed-operand spelling.

// #1150: a guard's fresh divergent throw (same type E1) propagates to the OUTER
// `catch E1` (rc=5), NOT the inner arm body (which would return 1).
BEZEL_TEST(throw_in_catch_guard_1097, guard_reraise_binding_propagates_to_outer_1150) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1 end E1; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new E1(); "
        "    catch ex: E1 when throw new E1() => return 1; "  // guard supersedes
        "    end; "
        "  catch E1 => return 5; "                      // enclosing handler
        "  end; "
        "  return 0; "
        "end Main; end C;"), 5);
}

// #1150 / §31.2.1: the superseding throw is NOT caught by a LATER arm of the
// SAME try — it skips the same-try `catch E1 => return 9` and is caught only by
// the ENCLOSING `catch E1` (rc=5). A regression that let the later same-try arm
// catch it would return 9.
BEZEL_TEST(throw_in_catch_guard_1097, guard_reraise_binding_skips_later_same_try_arm_1150) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1 end E1; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new E1(); "
        "    catch ex: E1 when throw new E1() => return 1; "  // supersedes
        "    catch E1 => return 9; "                    // same-try later arm — must NOT catch
        "    end; "
        "  catch E1 => return 5; "                      // enclosing handler catches
        "  end; "
        "  return 0; "
        "end Main; end C;"), 5);
}
