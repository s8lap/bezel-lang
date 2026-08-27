// fix #3391 — a member access on a UNIT (`()`) receiver is ill-formed and must
// be REJECTED, not silently left untyped (§4.7 *The unit type*, §8.1
// *Definition*, §26.5 *Resolution*, §30.3 *The `Task`, `Task<T>`, and
// `OwningTask<T>` types*, v0.283).
//
// THE RULE, established from the documents before the check was written:
//
//   * §30.3 declares the blocking escape hatch as `public method Wait();` —
//     with NO return type — and names `Result` as where the value is read:
//     "`Wait()` and `Result` are synchronous escape hatches"; "`OwningTask<T>`
//     exposes `Result`: reading it on a **completed** task in an owned-consuming
//     position transfers ownership of the result to the reader".
//   * §4.7: "`()` is the type with exactly one value, also written `()`. It is
//     the return type of methods declared without an explicit return type."
//     So `t.Wait()` has type `()` — which is exactly what sema already stamps
//     on it (fix #2819, ExprTypecheck_Calls.cpp).
//   * §8.1 gives every type `GetType`/`Equals`/`GetHashCode`/`ToString` (unit
//     reaches them through §8.6 boxing, and §8.5 fixes unit's `ToString` as
//     `"()"`). Nothing else is declared on `()`, and §26.5 draws extension
//     candidates from the receiver type — the unit type cannot be extended to
//     (§26.2). So `()`'s member surface is CLOSED at those four names.
//
// So the documents settle it: `r.Id` where `r: ()` is ill-formed, and a compile
// error. There is no lowering to write — there is no `Id` on `()` to lower.
//
// WHAT WENT WRONG. Both member-access resolvers end in a chain of
// known-complete gates, and the PRIMITIVE arm of that chain exists precisely
// because a primitive carries no user `TypeSymbol` and so slips past the
// known-complete-user-type gate (#2126). `Kind::Unit` has exactly the same
// shape and was simply never added. So `r.Id` on a `()`-typed `r` was left
// UNTYPED and UNREPORTED, and codegen's `lowerExpr` then had nothing to lower.
// Each enclosing statement took its own degradation path, all of them exit-0,
// ASan-clean and WRONG (measured on d05399c):
//
//   sum := sum + r.Id;   -> #2961 "assignment RHS expression failed to lower"
//                           — store DROPPED, `sum` printed 0 where 52 is right
//   var id := r.Id;      -> #2434 "initializer failed to lower" — slot
//                           zero-initialized
//   if r.Id <> 13 then   -> condition dropped; the arm never ran, so the `if`
//                           was VACUOUSLY true-looking
//   r.BogusCall();       -> accepted in FULL SILENCE; no diagnostic at all
//
// The last one is the tell: `r.TotallyBogusMember` and `r.BogusCall()` compiled
// clean, so this was never a lowering gap for a member that exists — it was a
// missing member-resolution gate. That also means the third shape had already
// cost the project a false-passing test: `fresh_receiver_deferred_iterator_
// dispose_2261`'s d4 guarded on `if r.Id <> 13 then return 99; end;` and passed
// because the condition was dropped, not because the field held 13 (updated in
// this same change to read `t.Result`).
//
// ⚠ This is a REJECTION, so the false-reject hazard dominates — T3 below is the
// accept-control block, and it is deliberately larger than the reject block.
//
// ONE PREDICATE, NOT A SECOND ONE. The fix does not add a parallel unit gate
// beside the primitive gate: it lifts the primitive gate's surface question
// into one file-local `closedValueSurfaceMemberReason` (ExprTypecheck_Calls.cpp)
// that BOTH the `.` resolver and the `?.` mirror (#3155) now ask, and adds the
// `()` arm there. T2 pins the `?.` spelling and T3d pins that the primitive arm
// still speaks its own wording, so the two cannot drift apart.
//
// TEST MATRIX:
//   T1  REJECTS — the issue's repro (field read off `t.Wait()` inside a loop),
//       plus the shapes it claimed were different: the SAME read outside a
//       loop, the no-intermediate-binding form `t.Wait().Id`, a METHOD call on
//       the unit value, and a `()` that never came from a task at all
//       (category-blindness).
//   T2  REJECT — the `?.` spelling on a `()?` receiver, the #3155 mirror.
//   T3  ACCEPT-CONTROLS — §8.1's `ToString` on a `()`; `var u: () := ();`
//       and `u = v` (the #2819 accept-controls, unchanged); the primitive arm's
//       own wording; an ordinary class member access.
//   T4  VALUE CONTROLS (run, assert a VALUE): the correct §30.3 spelling
//       (`t.Wait(); … t.Result`) computes the RIGHT answer — 13 for the field
//       read off a class result, 20 for the loop the issue reported. Pre-fix
//       the same programs' broken twin printed 0; asserting the value is what
//       separates "the store landed" from "the store was dropped".

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

// Parse + collect + resolve + typecheck. Sema only — nothing is compiled or
// run, so an assertion on the diagnostic text cannot be satisfied by a crash.
bezel::test::SemaResult semaAll3391(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag3391(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The exact wording the fix emits. Asserting the TEXT (not merely "an error
// occurred") is what makes this a contract on the diagnostic — and what makes
// it fail pre-fix, where NO diagnostic was emitted at all for these shapes.
constexpr std::string_view kUnitMember = "on the unit type `()`";
// The primitive arm's wording, which the shared predicate must keep producing.
constexpr std::string_view kPrimMember = "on primitive type `int`";

// The #3391 repro's declarations: a class result and an `async` producer whose
// `OwningTask<Res>` is settled with the blocking `Wait()` escape hatch (§30.3).
constexpr std::string_view kDecls =
    "module M; "
    "class Res "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  public method Get(): int return Id; end Get; "
    "end Res; "
    "class Client "
    "  public N: int := 13; "
    "  public async method Fetch(): owned OwningTask<Res> "
    "    await Task.Delay(1); "
    "    return new Res(this.N); "
    "  end Fetch; "
    "end Client; ";

std::string prog3391(std::string_view body) {
    return std::string(kDecls) +
        "class P "
        "  public static method Main(): int " +
        std::string(body) +
        "  end Main; "
        "end P;";
}

}  // namespace

// ===========================================================================
// T1 — REJECTS.
// ===========================================================================

// T1a — the issue's own repro, verbatim. `var r := t.Wait();` binds `r` to `()`
// (§30.3 + §4.7), so `r.Id` names nothing. Pre-fix: compiled, emitted only the
// #2961 store-dropped internal note, exited 0, and printed `sum=0` where 52 is
// the answer.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t1a_field_read_off_wait_in_loop_rejected_4_7_30_3) {
    auto r = semaAll3391(prog3391(
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      using var t := new Client().Fetch(); "
        "      var r := t.Wait(); "
        "      sum := sum + r.Id; "
        "      i := i + 1; "
        "    end while; "
        "    return sum; "));
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// T1b — the issue reported "the loop context looks load-bearing". It is not:
// the SAME read outside any loop degraded identically (measured on d05399c —
// the #2961 note at the assignment, `sum=0`). Pinning the non-loop form keeps a
// future fix from being scoped to loops.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t1b_field_read_off_wait_outside_loop_rejected_4_7) {
    auto r = semaAll3391(prog3391(
        "    var sum := 0; "
        "    using var t := new Client().Fetch(); "
        "    var r := t.Wait(); "
        "    sum := sum + r.Id; "
        "    return sum; "));
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// T1c — no intermediate binding: the member access hangs directly off the call.
// Category-blindness w.r.t. where the `()` is stored.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t1c_field_read_directly_off_wait_call_rejected_4_7) {
    auto r = semaAll3391(prog3391(
        "    var sum := 0; "
        "    using var t := new Client().Fetch(); "
        "    sum := sum + t.Wait().Id; "
        "    return sum; "));
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// T1d — a METHOD call on the unit value, not a field read. Both spellings reach
// `resolveMemberAccess`, and both were silent pre-fix.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t1d_method_call_on_wait_result_rejected_4_7) {
    auto r = semaAll3391(prog3391(
        "    var sum := 0; "
        "    using var t := new Client().Fetch(); "
        "    var r := t.Wait(); "
        "    sum := sum + r.Get(); "
        "    return sum; "));
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// T1e — the gate keys on the RECEIVER'S TYPE being `Kind::Unit`, not on
// `Wait()`. A `()` from the literal, with no task anywhere in the program, is
// rejected by the same predicate. (This is the shape that proves the fix is not
// a `Wait()`-shaped special case — the failure mode #2819's sibling gate was
// written to avoid.)
BEZEL_TEST(unit_receiver_member_surface_3391,
           t1e_member_on_unit_literal_local_rejected_4_7) {
    auto r = semaAll3391(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    var x := u.TotallyBogusMember; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// ===========================================================================
// T2 — the `?.` mirror (#3155). `()?` is a well-formed type (§4.7: "Wrapping
// the unit type with `?` … is permitted by the grammar"), and the null-
// conditional spelling had the same hole: no primitive arm matched a Unit
// receiver, so it fell through untyped and silent.
// ===========================================================================
BEZEL_TEST(unit_receiver_member_surface_3391,
           t2_nullconditional_member_on_unit_rejected_7_5) {
    auto r = semaAll3391(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: ()? := null; "
        "    var x := u?.Bogus; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kUnitMember));
}

// ===========================================================================
// T3 — ACCEPT-CONTROLS. A rejection's real risk is over-firing.
// ===========================================================================

// T3a — §8.1's universal members exist on EVERY type; §8.5 fixes unit's
// `ToString` as `"()"`, and codegen prints exactly that. The gate exempts all
// four (`ToString`/`Equals`/`GetHashCode`/`GetType`) by name, exactly as the
// primitive arm does; `ToString` is the one of the four that codegen can
// currently lower on a `()` receiver (`u.Equals(u)` and `u.GetHashCode()`
// reach the #3079 argument-failed-to-lower degradation — a separate codegen
// gap, #3422, and untouched either way by this rejection).
BEZEL_TEST(unit_receiver_member_surface_3391,
           t3a_universal_tostring_on_unit_still_accepted_8_1_8_5) {
    const std::string out = bezel::test::runAotOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    Console.WriteLine(u.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("()"));
}

// T3b — #2819's accept-controls, replayed: declaring, assigning and comparing a
// `()` are all legal (§4.7 makes unit a value type; §8.4 defines `=` as
// `Equals`, which unit has). None of these is a member ACCESS, so none may
// reach the new gate.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t3b_unit_local_and_equality_still_accepted_4_7_8_4) {
    auto r = bezel::test::runAot(
        "module M; "
        "class C "
        "  private static method Nop() end Nop; "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    var v: () := (); "
        "    u := (); "
        "    C.Nop(); "
        "    if u = v then return 7; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// T3c — a bare `t.Wait();` as a STATEMENT (its only correct use, §30.3) stays
// accepted, and the statements after it still run.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t3c_bare_wait_statement_still_accepted_30_3) {
    auto r = semaAll3391(prog3391(
        "    using var t := new Client().Fetch(); "
        "    t.Wait(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T3d — the shared predicate's PRIMITIVE arm still speaks its own stdlib §9
// wording. If the refactor had collapsed the two arms into one message, or
// dropped the primitive arm, this is what would catch it (#2126's contract).
BEZEL_TEST(unit_receiver_member_surface_3391,
           t3d_primitive_arm_wording_preserved_2126) {
    auto r = semaAll3391(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 5; "
        "    var x := i.BogusMember; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag3391(r, kPrimMember));
    BEZEL_EXPECT_FALSE(hasDiag3391(r, kUnitMember));
}

// T3e — an ordinary class member access is untouched. The blast-radius control
// for a gate that sits in the shared member-resolution tail.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t3e_ordinary_class_member_access_unaffected_11_3) {
    auto r = bezel::test::runAot(
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new Res(41); "
        "    return r.Id + 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T4 — VALUE CONTROLS. The correct §30.3 spelling computes the RIGHT answer.
// These are the tests that distinguish "the store landed" from "the store was
// dropped": pre-fix, the rejected twins of these programs ran to exit 0 and
// printed 0.
// ===========================================================================

// T4a — the repro's own class-result field read, spelled correctly: `Wait()` is
// the statement, `Result` is the value (§30.3). 13 is `Client.N`; the dropped-
// store answer was 0.
BEZEL_TEST(unit_receiver_member_surface_3391,
           t4a_result_field_read_yields_the_value_30_3) {
    const std::string out = bezel::test::runAotOut(std::string(kDecls) +
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var t := new Client().Fetch(); "
        "      t.Wait(); "
        "      var res := t.Result; "
        "      sum := sum + res.Id; "
        "    end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_EQ(out, std::string("sum=13"));
}

// T4b — the LOOP the issue reported, accumulating a real per-iteration value
// through `Result`. 2+4+6+8 = 20; the dropped-store answer was 0. (A `Task<int>`
// result keeps this control leak-free: reading `.Result` for a CLASS result
// inside a loop leaks the result today, #3420.)
BEZEL_TEST(unit_receiver_member_surface_3391,
           t4b_result_in_loop_accumulates_the_value_30_3) {
    const std::string out = bezel::test::runAotOut(
        "module M; "
        "class C "
        "  private static async method Work(i: int): Task<int> "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 1; "
        "    while i <= 4 do "
        "      using var t := C.Work(i); "
        "      t.Wait(); "
        "      total := total + t.Result; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("total=20"));
}
