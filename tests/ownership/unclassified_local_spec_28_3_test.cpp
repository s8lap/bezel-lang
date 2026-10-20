// Spec-derived regression tests for §28.3 (v0.339) — the `unclassified`
// auxiliary ownership state and its ADOPTION clause.
//
// v0.339 added a FOURTH auxiliary ownership state alongside `uninitialized`,
// `polymorphic-null` and `explicitly-disposed` (K1, cited by §28.3's
// "Ownership state of locals"):
//
//   "**unclassified** — a class- or interface-typed local whose every value so
//    far is runtime-managed (K1.6): `var o: Object := 7;`, `var o: Object :=
//    "s";` — a boxing conversion (§8.6) or another runtime-managed
//    production, the Producers table's runtime-managed row; a value that holds
//    borrows is never boxed at a local (§8.6). It carries no obligation and no
//    source, so a read of it is no borrow (§30.5's runtime-managed operand); it
//    adopts the state of the first classified assignment that reaches it, by
//    the same clause."
//
// and §28.3's ADOPTION clause:
//
//   "Three states qualify, and no others: an `uninitialized` local …, one in
//    the polymorphic null state (…), and an `unclassified` local …. The
//    adopted state is that of the first assignment that reaches the local **on
//    each path** — owned from a fresh producer *or a marked transfer*, borrowed
//    from an unmarked binding read or any other borrow; … for an unclassified
//    LHS, of the first **classified** such assignment, a runtime-managed RHS
//    deciding nothing and the displaced runtime-managed value disposed by
//    nothing."
//
// and its closing sentence, which is the one half these tests pin as an
// ACCEPTANCE:
//
//   "An unclassified local that is never classified stays outside the ownership
//    rules, as a `string` local does: nothing disposes it and its reads borrow
//    from nothing."
//
// §30.5's producer-side rules restate the same fact as a named example —
// "a runtime-managed operand — a `string`, a box under `Task<Object>`, an
// **unclassified** local holding one (`var o: Object := 7; return o;` […]) is
// no borrow and is not reached". That spelling is NOT compiled by any row in
// this file, and the reason is in the MEASURED block below and at the foot of
// the file: the §30.5 kind-1 check does not run at `Task<Object>`, so a row
// there would assert nothing.
//
// ── WHAT WAS MEASURED, AND WHERE ───────────────────────────────────────────
//
// MEASURED on this tree (`58e87f6f`, spec v0.339 in the working tree) with
// `build/bezelc -c`, every arm re-run by hand; the numbers below are the ones
// the assertions in this file encode.
//
//   * The compiler DOES carry the `unclassified` state and DOES apply the
//     adoption clause. There is no fourth enumerator named `Unclassified` in
//     `src/ownership/OwnershipState.hpp` (the enum holds `Uninitialized`,
//     `Owned`, `Borrowed`, `MovedFrom`, `Disposed`, `PolyNull`, `Poisoned`),
//     so the state is not modelled by name; it is reachable as the "holds no
//     classified value" condition of the ADOPTION clause, which is what §28.3
//     actually specifies ("Three states qualify, and no others" is a rule about
//     the ANALYSIS, not about an enumerator list). What the tests below pin is
//     the observable consequence, which is what the spec constrains.
//
//   * THE BOX IS DOING THE WORK — this is what makes the rows load-bearing.
//     Four programs, all `Object`-typed, differing only in how the local
//     reaches an owned value:
//
//         var o: Object := new Res(1);              rc=0   (direct init)
//         var o: Object := new Res(1);
//                    o := new Res(2);                rc=0   (SECOND assignment)
//         var o: Object := 7;
//                    o := new Res(1);                rc=1   (BOX then assign)
//         var o: Object := "s";
//                    o := new Res(1);                rc=1   (STRING then assign)
//
//     rc=1 carries `:6:5: owned value in 'o' would be dropped here without
//     disposal (§28.3 …)`. Direct init and a second owned-to-owned assignment
//     are both silent — the first is adopted straight from the initializer, the
//     second disposes the displaced value per §28.7's drop-on-overwrite. Only
//     the box/string-then-assign sequence flips, because only there does the
//     local hold NO classified value at the assignment, so the assignment
//     ADOPTS instead of discharging — and the adopted owned value then owes a
//     disposal nothing supplies. The two rc=0 rows are the control that makes
//     the two rc=1 rows mean "the box is what did it" rather than "an
//     `Object`-typed local is under-discharged".
//
//   * §30.5's named example `var o: Object := 7; return o;` under `Task<Object>`
//     is ACCEPTED (rc=0), but NO ROW HERE ASSERTS IT. The §30.5 kind-1 check is
//     not evaluated at `Task<Object>` at all:
//     `MethodOwnership::checkAsyncPatternABorrowedIdentifierReturn`
//     (src/ownership/Ownership.cpp:15369-15373) returns immediately unless
//     `isAsyncPatternAClassResultType(...)` (same file, 15308-15340), and that
//     predicate deliberately excludes `Object` — its own comment at 15322-15326
//     records the reason ("this identifier check cannot tell a box from an
//     object reference") and defers the gap to #5704. MEASURED controls, same
//     program with only the return type swapped:
//
//         Task<Res>  (destructor class), `return p;`      rc=1  — kind 1 fires
//         Task<Object>, same parameter                    rc=0
//         Task<Object>, owned local `return r;`           rc=0
//         Task<Object>, `return new Res();`               rc=0
//
//     So an acceptance row there would stay green if the compiler had no
//     concept of `unclassified` at all, and would stay green if §30.5 kind 1
//     were deleted wholesale. A green row there is false assurance, and #5704
//     — not this file — is where that gap is tracked and belongs.
//
// ── WHAT IS NOT PINNED HERE, AND WHY ───────────────────────────────────────
//
// The JOIN half of the adoption clause ("At a join where one edge leaves the
// local unclassified and another leaves it classified, a borrowed edge's state
// is taken … and an owned edge is K7.3's mixed merge, rejected") is NOT pinned:
// the branch-join lattice `joinInto` (src/ownership/Ownership_Checks.cpp) keys
// on the `OwnershipState` enumerator, and with no `Unclassified` member a
// mixed-merge shape would need a probe shape that distinguishes "rejected
// because of the unclassified edge" from "rejected for some other reason". No
// such shape was found inside this lane's window, and a test that pinned the
// wrong one would be a test asserting the compiler's behaviour rather than the
// spec's. Left explicitly open rather than guessed at.
//
// Also NOT pinned: §28.3's self-assignment sentence ("A self-assignment is no
// exception: for an owned-state class-typed local `t`, `t := t;` … is
// rejected"). MEASURED: the compiler ACCEPTS it (rc=0, only the pre-existing
// auto-drop warning) — already filed as #6609, out of this lane's scope, and a
// test expecting that rejection would fail. See the handback.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAot` only (the strict AOT oracle, per tests/README.md). No
// `runAotChecked` / `runAotCheckedLenient` / `runAotCheckedDisposal` call site
// is added here — `scripts/check_toolchain_skip_surface.py` counts those tokens
// by raw text and raising its denominator without a fall in `absorbing_sites`
// refuses the bank. Each acceptance assertion also checks `r.ran` and
// `exitCode == 0`, so the arms are not satisfied by a program that merely
// failed to compile.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §28.3's own spellings. `kRes` is a trivial-dispose class so the obligation
// question is not entangled with a destructor ceremony.
constexpr const char* kTypes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "end Res; "
    "public class Program ";

// The body runs to a clean exit; each test asserts stdout where the arm prints,
// so "accepted" is never satisfied by a program that failed to build.
constexpr const char* kMainTail =
    "  public static method Main(): int "
    "    Console.WriteLine(0); "
    "    return 0; "
    "  end Main; "
    "end Program;";

std::string program(const std::string& body) {
    return std::string(kTypes) + "  " + body + kMainTail;
}

// Accepted, with zero diagnostics of any severity, and it RUNS.
void expectCleanRun(const std::string& src) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Rejected at a real source location with a named §28.3 discharge diagnostic.
// `failedAtCompileTimeWithRealLocation` (not a bare `hasErrors`) so a compiler
// crash or a harness failure cannot satisfy the arm.
void expectDischargeError(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "would be dropped here without disposal"));
}

}  // namespace

// ── Rule 2, half 1: the ADOPTION clause ────────────────────────────────────
//
// §28.3 (v0.339): "for an unclassified LHS, of the first **classified** such
// assignment". MEASURED: `var o: Object := 7;` leaves the local holding no
// classified value, and the NEXT assignment — a fresh producer — adopts
// `Owned`, which brings the §28.3 owned-local discharge requirement with it:
// the program is REJECTED because the adopted owned value is never discharged.
//
// This is the load-bearing arm for rule 2: the box initializer alone is silent
// (the next test), and adding exactly one `o := new Res();` flips rc 0 → 1 with
// a real §28.3 diagnostic at `:7:5`. That difference IS the state, observed
// from outside; no enumerator is named anywhere in this file.
BEZEL_TEST(unclassified_local_spec_28_3,
           unclassified_box_local_adopts_owned_28_3) {
    expectDischargeError(runAot(program(
        "public static method Run(): int "
        "  var o: Object := 7; "
        "  o := new Res(1); "
        "  return 0; "
        "end Run; ")));
}

// ── Rule 2, half 1, the control arm: PARSES, and discharges cleanly ───────
//
// The control that makes the arm above a measurement. Identical program, one
// `.Dispose()` added: rc=0, zero diagnostics, runs. So the rejection above is
// the §28.3 discharge requirement firing on an ADOPTED owned value, not a
// syntax error, not a type error, and not an unrelated check that happens to
// sit at the same line.
BEZEL_TEST(unclassified_local_spec_28_3,
           unclassified_box_local_adopts_owned_discharged_28_3) {
    expectCleanRun(program(
        "public static method Run(): int "
        "  var o: Object := 7; "
        "  o := new Res(1); "
        "  o.Dispose(); "
        "  return 0; "
        "end Run; "));
}

// ── Rule 2, half 2: never classified ⇒ outside the ownership rules ─────────
//
// §28.3 (v0.339): "An unclassified local that is never classified stays outside
// the ownership rules, as a `string` local does: nothing disposes it and its
// reads borrow from nothing."
//
// MEASURED: rc=0, zero diagnostics. Compare with the first test, which is this
// program plus one classified assignment; the pair is the measurement, and this
// arm alone would prove nothing.
BEZEL_TEST(unclassified_local_spec_28_3,
           unclassified_never_classified_carries_no_obligation_28_3) {
    expectCleanRun(program(
        "public static method Run(): int "
        "  var o: Object := 7; "
        "  return 0; "
        "end Run; "));
}

// The `string` twin, REJECTING arm — §28.3 names `var o: Object := "s";` beside
// `var o: Object := 7;` as the two spellings of the same runtime-managed
// production, so the two must behave identically. MEASURED: rc=1, `:6:5: owned
// value in 'o' would be dropped here without disposal (§28.3 …)`, the same
// diagnostic at the same location as the boxed arm above. This is what gives the
// string spelling its pair-purchase: without it the string row below pins only
// the accepted half, and a compiler that adopted nothing for a `string` would
// still pass it.
BEZEL_TEST(unclassified_local_spec_28_3,
           unclassified_string_local_adopts_owned_28_3) {
    expectDischargeError(runAot(program(
        "public static method Run(): int "
        "  var o: Object := \"s\"; "
        "  o := new Res(1); "
        "  return 0; "
        "end Run; ")));
}

// The `string` twin, ACCEPTED arm — the same program with the adopted owned
// value discharged. MEASURED: rc=0, zero diagnostics, runs. Its control arm is
// the rejecting row immediately above.
BEZEL_TEST(unclassified_local_spec_28_3,
           unclassified_string_local_adopts_owned_discharged_28_3) {
    expectCleanRun(program(
        "public static method Run(): int "
        "  var o: Object := \"s\"; "
        "  o := new Res(1); "
        "  o.Dispose(); "
        "  return 0; "
        "end Run; "));
}

// ── NOT PINNED HERE: §30.5's runtime-managed operand ───────────────────────
//
// §30.5 cites the exact spelling — "an **unclassified** local holding one
// (`var o: Object := 7; return o;`, §28.3's Producers table's runtime-managed
// row) is no borrow and is not reached" — so it is tempting to pin the
// acceptance. DO NOT. The §30.5 kind-1 check is not evaluated at `Task<Object>`
// at all: `checkAsyncPatternABorrowedIdentifierReturn` returns immediately
// unless `isAsyncPatternAClassResultType(...)`, and that predicate deliberately
// excludes `Object` (src/ownership/Ownership.cpp:15322-15326, deferring the gap
// to #5704). MEASURED, same program with only the return type swapped:
//
//     Task<Res> (destructor class), `return p;`     rc=1  — kind 1 fires
//     Task<Object>, the same parameter               rc=0
//     Task<Object>, owned local `return r;`          rc=0
//     Task<Object>, `return new Res();`              rc=0
//
// An acceptance row there would stay green if the compiler had no concept of
// `unclassified` at all, and would stay green if §30.5 kind 1 were deleted
// wholesale — false assurance, and #5704 is where that gap is tracked.
