// Spec-derived tests for §31.1's "nullability not admitted" clause on the
// throw operand (issue #3076).
//
// §31.1 (v0.243, proposal 0080, #3046): "`throw expr` requires `expr` to be
// an **owned instance of an `Exception`-derived class type**: the operand's
// static type, **nullability not admitted**, must be `Exception` or a class
// deriving from it". The "Exception`-derived" half of the sentence landed
// with #3046 (`ExprTC::checkThrowableType` in `src/sema/ExprTypecheck.cpp`
// tests `isExceptionDerivedClass(vt)`); the "nullability not admitted" half
// was deferred (#3076) because `ctx_.exprType()` reports the DECLARED type,
// while §7.4 flow narrowing lives in this pass's own `nullNarrowed_` /
// `narrowings_` side-sets — a bare `vt->nullable` reject would fire on the
// §7.4-admitted `if e <> null then throw e; end;` shape.
//
// The fix extends `checkThrowableType` with a nullable-operand branch that
// is suppressed when the operand is provably non-null at the throw site
// (§7.4 narrowing, the `!` operator, or a non-nullable declared type). The
// cases that pin both the reject half and the accept controls:
//
//   T1 (rejection, §31.1 "nullability not admitted"): a `throw e` whose
//       declared type is nullable (`AppError?`) and that has NOT been
//       narrowed non-null at the throw site is rejected.
//
//   T2 (ACCEPT control, non-nullable declared type): `throw e` where `e` is
//       declared `AppError` (non-nullable) — the original #3046 ACCEPT shape.
//
//   T3 (ACCEPT control, §7.4 null-narrowing): `if e <> null then throw e;
//       end;` — the §7.4-admitted narrowing shape. The fix MUST NOT
//       over-reject this; the reject was deliberately deferred from #3046
//       precisely because a `vt->nullable` check alone would fire here.
//
//   T4 (ACCEPT control, `!` null-forgiving operator): `throw e!` —
//       `PostfixBangExpr`'s exprType is the non-nullable form of the
//       operand's type, so `vt->nullable == false` already admits it; the
//       fix's nullable-branch is irrelevant for this shape (the path that
//       admits it is the `if (!vt->nullable) return;` short-circuit, NOT the
//       narrowing-consultation path). The pin is to guarantee that an
//       over-aggressive narrowing-only fix (one that drops the
//       `vt->nullable == false` short-circuit and requires narrowing even
//       for `e!`) would FAIL this test.
//
//   T5 (ACCEPT control, narrowed local + throw at top level): a local
//       declared nullable is reassigned a non-null value, and the throw
//       follows the assignment on the same control-flow path. The
//       `narrowOnAssignment` path populates `nullNarrowed_` for the local,
//       so the narrowing-consultation path admits the throw.
//
//   T6 (rejection, method-returned nullable exception): `throw MakeNullable()`
//       where `MakeNullable(): AppError?` — the call result is an
//       `Exception`-derived nullable value, but it has no symbol to consult
//       for narrowing, so the rejection fires. This pins the "no
//       narrowing fact ⇒ reject" shape and confirms the fix does not
//       silently accept an un-narrowed returned value.
//
// Every reject test pins §31.1 as the diagnostic authority; every accept
// test pins the same source-shaped control with the nullable marker
// removed (or the narrowing added) so a regression in the predicate can
// not be hidden by either side shifting together.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// `AppError : Exception` is the throwable; `C.Main()` carries the throw
// site. Every test below wraps a body fragment so the prelude stays out
// of the test code.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class AppError : Exception "
        "  public constructor() base(\"e\"); end constructor; "
        "end AppError; "
        "class C "
        "  public static method Make(): AppError? => null; "
        "  public static method MakeNonNull(): AppError => new AppError(); "
        "  public static method Main(): int ") +
        method + "end Main; "
        "end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// T1 (rejection, §31.1): `throw e` where `e` is declared `AppError?` and is
// not narrowed at the throw site — the bug shape that #3046 silently
// accepted. After fix #3076 the diagnostic cites §31.1 and explains the
// nullable-operand rule.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           plain_nullable_throw_rejected_sec_31_1) {
    auto r = parseSemaAll(wrap(
        "  var e: AppError? := Make(); "
        "  throw e; "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "the throw operand's type is nullable (§31.1"));
}

// ---------------------------------------------------------------------------
// T2 (ACCEPT control, §31.1 #3046): `throw e` where `e` is declared
// `AppError` (non-nullable) — the originally-accepted #3046 shape. The fix
// MUST NOT over-reject this; the short-circuit `if (!vt->nullable) return;`
// path is what admits it.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           nonnullable_throw_accepted_control) {
    auto r = parseSemaAll(wrap(
        "  var e: AppError := MakeNonNull(); "
        "  throw e; "
        "  return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T3 (ACCEPT control, §7.4 null-narrowing): `if e <> null then throw e;`
// — the §7.4-narrowed shape. The fix MUST NOT over-reject this; the
// narrowing-consultation path (`isKnownNonNull(sym)` via `nullNarrowed_`)
// is what admits it. The bug that #3076 was filed against — and the reason
// the fix was deferred from #3046 — is precisely that a bare
// `vt->nullable` reject would have fired here.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           null_narrowed_throw_accepted_control_sec_7_4) {
    auto r = parseSemaAll(wrap(
        "  var e: AppError? := Make(); "
        "  if e <> null then "
        "    throw e; "
        "  end; "
        "  return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T4 (ACCEPT control, `!` null-forgiving operator): `throw e!` — the
// postfix `!` rewrites the operand's exprType to its non-nullable form per
// `visitExpr`'s `PostfixBangExpr` arm (see
// `src/sema/ExprTypecheck_Exprs.cpp` ~line 1909). The fix's
// `if (!vt->nullable) return;` short-circuit is what admits this case.
// An over-aggressive narrowing-only fix (one that demands a narrowing
// fact even for `e!`) would FAIL this test — pinning the short-circuit
// shape is part of the pin.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           postfix_bang_throw_accepted_control) {
    auto r = parseSemaAll(wrap(
        "  var e: AppError? := Make(); "
        "  throw e!; "
        "  return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T5 (ACCEPT control, narrowed-after-assignment): a local declared nullable
// is reassigned a non-null value, and the throw follows the assignment on
// the same control-flow path. `narrowOnAssignment` populates
// `nullNarrowed_` for the local (see `src/sema/ExprTypecheck.cpp` ~line
// 8595), so the narrowing-consultation path admits the throw. (Bezel
// permits local reassignment, unlike the `const` shape the brief
// considered — see the comment on `narrowOnAssignment` for the asymmetry.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           narrowed_after_assignment_throw_accepted_control_sec_7_4) {
    auto r = parseSemaAll(wrap(
        "  var e: AppError? := Make(); "
        "  e := MakeNonNull(); "
        "  throw e; "
        "  return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T6 (rejection, method-returned nullable exception): `throw MakeNullable()`
// where `MakeNullable` is declared `: AppError?`. The call result is an
// `Exception`-derived nullable value, but it has no symbol to consult for
// narrowing (`ctx_.exprSymbol(callResult)` is null for an arbitrary call),
// so the rejection fires. This pins the "no narrowing fact ⇒ reject" shape
// and confirms the fix does not silently accept an un-narrowed returned
// value.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_throw_nullable_operand_3076,
           method_returned_nullable_throw_rejected_sec_31_1) {
    auto r = parseSemaAll(wrap(
        "  throw Make(); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "the throw operand's type is nullable (§31.1"));
}