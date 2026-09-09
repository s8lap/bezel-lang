// Spec-derived regression test for issue #3912.
//
// §7.4 (Member access on nullable values), v0.294.
//
// §7.4 is verbatim: "Member access, method calls, and indexing on a nullable
// value are compile errors unless the compiler has narrowed it to non-null."
// `resolveMemberAccess` (ExprTypecheck_Calls.cpp) enforced this with a
// three-arm `if`/`else if` keyed on `ma.target`'s AST node kind:
// IdentifierExpr (a bare local/parameter, narrowing-tracked), MemberAccessExpr
// (a field/property chain, #862), and IndexExpr (an indexer result, #1161).
// There was no default arm, so a `ma.target` of any OTHER node kind fell
// through the whole gate untouched and typed as if it were non-nullable.
//
// The filed repro is a direct CALL result: `N().Length` where
// `N(): string?`. `ma.target` is a `CallExpr`, matched none of the three
// arms, and the member access compiled clean; codegen then dereferenced the
// (possibly null) return value directly — SIGSEGV at runtime. Contrast the
// character-identical local-variable spelling (`var n := N(); n.Length`),
// which the IdentifierExpr arm correctly rejects.
//
// FIX #3912 replaces the three-arm chain's implicit "do nothing" default
// with an explicit `else` that applies the exact same "not a local, not
// narrowable — reject a nullable-typed result outright" rule the IndexExpr
// arm already applies, to whatever OTHER expression kind `ma.target` turns
// out to be. This is deliberately not a fourth named arm for `CallExpr`
// alone: the missing-default shape is the actual defect, and enumerating
// node kinds one arm at a time only postpones the next missed one. The same
// catch-all is confirmed here to also close three sibling shapes that were
// equally unguarded before this fix and share the identical reasoning: a
// conditional atom (§20) result, a `??` (§7.7) result, and an `await`
// (§30) result. `as` (§17.4) is the FOURTH such shape — pinned instead in
// the existing `codegen_expr_member` / `arg_position_box_release_2952` /
// `array_record_boxed_collection_disposal_2791` / `property_accessor_types_510`
// suites, three of which needed a `!` added in the same commit as this fix
// once the gate started (correctly) rejecting their pre-existing unguarded
// `(x as T).member` reads.
//
// Every negative case here asserts on diagnostic TEXT (not the coarse
// hasErrors() predicate, which cannot distinguish a genuine compile-time
// rejection from a runtime SIGSEGV that runAot's WIFSIGNALED path
// synthesizes as an Error diagnostic too — see tests/README.md). The positive
// controls are the load-bearing half: the fix must not touch a non-nullable
// call result, must not touch the pre-existing IdentifierExpr/IndexExpr arms,
// and must still admit the standard `!`-forgiving remedy on a call result.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, fix #3932
#include "support/aot_support.hpp"  // runAot — end-to-end accept/crash control
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The new catch-all's diagnostic fragment — distinguishes it from the
// pre-existing IdentifierExpr arm's "on a nullable value requires narrowing"
// and the MemberAccessExpr/IndexExpr arms' "on a nullable field or
// property"/"on a nullable index result" wordings, so a sibling gate cannot
// satisfy this assertion by accident.
constexpr const char* kNullableResultDiag =
    "on a nullable expression result requires narrowing";
constexpr const char* kSec74 = "(\xC2\xA7""7.4)";

std::string prog(const std::string& body) {
    return std::string(
               "module M; "
               "public class C "
               "  private static method N(): string? => null; "
               "  private static method NN(): string => \"ok\"; "
               "  private static async method NA(): Task<string?> => null; "
               ) + body + " end C;";
}

}  // namespace

// #3912 exact repro — a direct CallExpr receiver whose declared return type
// is nullable, member-accessed with no narrowing.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           direct_call_result_field_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  const w := N().Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #3912 — the issue also names a METHOD CALL on the same shape
// (`N().CompareTo("x")`): the outer CallExpr's callee is a MemberAccessExpr
// whose OWN target is the inner CallExpr `N()` — the same gate must fire on
// that inner member access before the outer call is even resolved.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           direct_call_result_method_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  const w := N().CompareTo(\"x\"); "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #3912 sibling shape 1 — a conditional atom (§20) result. Both arms are
// nullable, so the whole `(if .. then .. else ..)` is nullable; direct
// member access without narrowing is the identical defect.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           conditional_atom_result_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var k := true; "
        "  const w := (if k then N() else N()).Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #3912 sibling shape 2 — a `??` (§7.7) result that STAYS nullable because
// both operands are nullable (`T? ?? T?` is `T?`). Direct member access
// without narrowing is the identical defect.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           coalesce_result_still_nullable_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  const w := (N() ?? N()).Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #3912 sibling shape 3 — an `await` (§30) result of a `Task<T?>`. Direct
// member access without narrowing is the identical defect.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           await_result_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static async method Main(): Task<int> "
        "  const w := (await NA()).Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// ACCEPT control — a NON-nullable call result must not be touched by the
// new catch-all. Pins that the fix gates on `tgt->nullable`, not on "target
// is not Identifier/MemberAccess/Index".
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           nonnullable_call_result_accepted) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  return NN().Length; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "ok".Length
}

// ACCEPT control (§7.8 force-unwrap carve-out) — `N()!.Length` is a
// PostfixBangExpr target, already non-nullable at this point, so the
// catch-all's `tgt->nullable` check does not fire. Confirms the standard
// remedy the diagnostic names actually round-trips through the new gate.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           force_unwrap_call_result_admitted_7_8) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  return N()!.Length; "
        "end Main;"));
    // N() returns null, so the compile is clean but the force-unwrap raises
    // NullReferenceException at runtime (§7.8) — the point being verified
    // is the ABSENCE of a compile-time diagnostic, not the exit code.
    BEZEL_EXPECT_TRUE(r.ran);
    (void)r.exitCode;
}

// REGRESSION GUARD — the pre-existing IdentifierExpr arm (a bare local of
// nullable type) must still reject exactly as before; the new catch-all
// must not change its wording or displace it.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           local_variable_form_still_rejected_7_4_unchanged_wording) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var n := N(); "
        "  const w := n.Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, "on a nullable value requires narrowing"));
    BEZEL_EXPECT_FALSE(hasError(r, kNullableResultDiag));
}

// =========================================================================
// #3932 — a NullCondMemberExpr (`x?.Y`) sibling shape of the #3912 bug
// class, discovered while reviewing commit 0468fc2 (this file's own fix).
//
// `resolveNullCondMember` (ExprTypecheck_Calls.cpp, ~line 22413) types a
// `?.` access UNCONDITIONALLY nullable per §7.5 ("the result type is always
// nullable" — the whole `?.` access yields `null` when the receiver is
// null, regardless of the accessed member's own nullability):
//
//   if (memberTy->nullable) {
//       ctx_.setExprType(&nm, memberTy);  // already nullable
//   } else {
//       TypeRef* nn = ctx_.makeTypeRef(*memberTy);
//       nn->nullable = true;
//       ctx_.setExprType(&nm, nn);
//   }
//
// So `N()?.Length` (where `N(): string?` and `.Length` is `int`, itself
// non-nullable) has exprType `int?` — and a FURTHER plain `.` access on
// that result (`N()?.Length.ToString()`) reaches `resolveMemberAccess`
// with `ma.target` a NullCondMemberExpr. `NullCondMemberExpr` is NOT a
// subtype of `MemberAccessExpr` (a sibling `Expr` node, ast/Node.hpp), so
// this does not match the MemberAccessExpr arm above — it falls to the
// #3912 catch-all `else` arm, whose `tgt->nullable` check DOES fire here
// (`tgt` is the lifted `int?`), correctly rejecting the access. This is
// the CORRECT #3912-class rejection (same class of bug #3912 fixes: a
// nullable-typed, non-narrowable expression result reaching a further
// plain `.member` unguarded) — it was a SILENT side effect of 0468fc2's
// fix, with no dedicated test and a commit-message/in-code comment that
// INCORRECTLY claimed NullCondMemberExpr targets "are already non-nullable
// at this point ... and never trip the check below" (fixed by this commit,
// see the corrected comment at the #3912 catch-all `else` arm above).
//
// TEST-GAP issue, not a behavior fix (LANE_RULES §3 applies with no
// "without" side in src/ — the rejection already exists on HEAD, verified
// against the real `build/bezelc -c` before writing these rows). Written
// on `runAot`, per LANE_RULES §3, matching this file's own existing
// `runAot`-based accept-side controls above.

// #3932's own measured repro, verbatim: `N()?.Length.ToString()` where
// `N(): string?` and `string.Length: int` (non-nullable) — the `?.`
// lifts `Length`'s access to `int?`, and the further unguarded `.ToString`
// must be rejected exactly like the #3912 sibling shapes.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           null_cond_member_further_plain_access_rejected_7_4_7_5) {
    auto r = runAot(prog(
        "public static method Main(): int "
        "  const w := N()?.Length.ToString(); "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNullableResultDiag));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSec74));
}

// ACCEPT control — the `?.` access ALONE (no further plain member access)
// must keep compiling: `N()?.Length` by itself is the ordinary, admitted
// null-conditional idiom (§7.5) and is untouched by the catch-all (nothing
// ever accesses `.Length`'s OWN result further here). Pins that the
// #3912-class rejection fires only on the FURTHER unguarded `.member`, not
// on the `?.` form by itself.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           null_cond_member_alone_accepted_7_5) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  const w := N()?.Length; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ACCEPT control — a further access GUARDED by a second `?.` (`N()?.Length
// ?.ToString()` spelled with the standard remedy the diagnostic names,
// `x?.Y?.Z`) must compile and run: the standard null-conditional chaining
// idiom is unaffected by the #3912-class rejection of the UNGUARDED form.
BEZEL_TEST(sema_call_result_nullable_receiver_3912,
           null_cond_member_chained_guard_accepted_7_5) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  const w := N()?.Length?.ToString(); "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
