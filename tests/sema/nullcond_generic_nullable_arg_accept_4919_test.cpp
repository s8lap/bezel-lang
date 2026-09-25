// fix #4919 (§7.5 / §14.1 / §7.10, v0.307): the null-conditional
// member-call path (`recv?.M(args)`) checked arguments against the
// UNSUBSTITUTED bare-T formal parameter type. The plain `.` member-call
// path applies the receiver class's type-parameter substitution
// (§14.1 — T := concrete type arg on the receiver) to each formal
// parameter type before the §5.1 / §7 assignability check; the `?.` path
// did not, because `ExprTC::checkCallArity`'s `recvTarget` extraction
// keyed on `MemberAccessExpr` only and left `recvTarget = nullptr` for a
// `NullCondMemberExpr` callee — leaving `checkArgsAgainstParams`'s
// `if (paramTy && classTarget && classParent) paramTy = substituteMemberType(...)`
// guard unsatisfied and the bare `T` formal unsubstituted. `checkAssignable`
// then saw `from = int?`, `to = bare T (non-nullable)` and rejected with
// the §7 "nullable but the target type is non-nullable" diagnostic —
// over-rejecting a working program whose bare-T formal IS the receiver's
// substituted T (e.g. `int?` on `Holder<int?>`).
//
// The fix extracts `recvTarget` from `NullCondMemberExpr` callees as well
// as `MemberAccessExpr` callees, so the same class-level substitution runs
// on the `?.` path. The same gap closed the corresponding RETURN-TYPE
// substitution site (the call's `rt` would have stayed the bare `T` and
// codegen emitted malformed IR on the substituted-return path). The
// substitution is gated to `classTarget && classParent` (and
// `parent->genericParams` non-empty, via `substituteMemberType`'s own
// early-out), so non-generic receivers are unaffected — only the gap is
// closed, nothing broader.
//
// Four rows pin the gap and its boundaries:
//
//   (1) ACCEPT  — the issue's repro: `h?.IdB(x)` on `Holder<int?>?` with
//                 `x: int?`; pre-fix REJECTS (the §7 "nullable but the
//                 target type is non-nullable" diagnostic), post-fix
//                 ACCEPTS (the program compiles and runs). The issue's
//                 expected runtime value `-1` is `0` on origin/main
//                 because #4899 (a codegen-side nullable-carrier fix) is
//                 not yet merged; the semantic acceptance is what this
//                 fix owns, and `r.ran + exitCode == 0` pins that without
//                 coupling the test to a sibling that lands separately.
//   (2) ACCEPT  — control: `hh.IdB(x)` on `Holder<int?>` (plain `.`); both
//                 pre- and post-fix ACCEPT (the gate the fix already
//                 passes). Same `#4899`-dependent runtime observation as
//                 row 1; the assertion is the program's compile+run
//                 success, not a specific stdout value.
//   (3) ACCEPT  — control: `h?.IdB(42)` on `Holder<int?>?` with a
//                 NON-nullable literal arg; both pre- and post-fix ACCEPT
//                 (the receiver-substituted `int?` accepts `int` via §5.1
//                 widening; the literal is non-null so `#4899` is not on
//                 this path's critical flow and the runtime prints `42`).
//   (4) REJECT  — control: `h?.IdB(x)` on `Holder<int>?` with `x: int?`
//                 (non-nullable receiver, nullable arg); both pre- and
//                 post-fix REJECT (§7: T = int, x = int?, incompatible) —
//                 proving the fix does not become a "permit any nullable
//                 arg" predicate.
//
// Per [[feedback-reviewer-must-run-accept-rows]] ★★★ the ACCEPT rows
// drive `runAot` (the strict AOT oracle), not the JIT path.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Drive BOTH resolveTypes and typecheckExpressions — the diagnostic the
// issue cites ("the argument `v` to call to `IdB` is nullable but the
// target type is non-nullable (§7)") lives in sema's argument check
// (checkArgsAgainstParams → checkAssignable), so this is the right
// pass. A parseAndCollect + Driver::compileFile path would also work,
// but parseSemaAll stays in-process and is what the issue's repro needs.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// §7 "nullable -> non-nullable" diagnostic fragment, shared by the
// user-method and builtin-collection paths. Identical wording for the
// argument in `checkArgsAgainstParams`'s plain `.` and `?.` arms — so the
// issue's repro and the (4) control's expected rejection both match.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// (1) ACCEPT — the issue's exact repro. Pre-fix REJECTS (sema emits the
// kNullableDiag and the program does not reach codegen); post-fix
// ACCEPTS (the program compiles and runs). The issue's expected runtime
// value `-1` is `0` on origin/main because #4899 (a codegen-side
// nullable-carrier fix on `buildOrderedArgs` / `lowerCall`) is not yet
// merged; the SEMANTIC acceptance is what this fix owns, so the
// assertions are `r.ran` + `exitCode == 0`, not a specific stdout value.
BEZEL_TEST(nullcond_generic_nullable_arg_accept_4919,
           issue_repro_h_dot_IdB_x_on_Holder_intQ_accepts_and_runs) {
    auto r = runAot(
        "module M4919; "
        "public class Holder4919<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T return v; end IdB; "
        "end Holder4919; "
        "public class C4919 "
        "  public static method Main(): int "
        "    using var hh := new Holder4919<int?>(); "
        "    var h: Holder4919<int?>? := hh; "
        "    var x: int? := null; "
        "    Console.WriteLine(h?.IdB(x) ?? -1); "
        "    return 0; "
        "  end Main; "
        "end C4919;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (2) ACCEPT — control: the byte-identical call through the plain `.`
// member-call path. Both pre- and post-fix accept and the program runs
// cleanly. Locks in the gate the fix already passes, so a regression
// that BREAKS the plain `.` path surfaces here too. Same `#4899`-
// dependent runtime observation as row 1; the assertion is the
// program's compile+run success.
BEZEL_TEST(nullcond_generic_nullable_arg_accept_4919,
           plain_dot_IdB_x_on_Holder_intQ_accepts_and_runs_control) {
    auto r = runAot(
        "module M4919dot; "
        "public class Holder4919dot<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T return v; end IdB; "
        "end Holder4919dot; "
        "public class C4919dot "
        "  public static method Main(): int "
        "    using var hh := new Holder4919dot<int?>(); "
        "    var x: int? := null; "
        "    Console.WriteLine(hh.IdB(x) ?? -1); "
        "    return 0; "
        "  end Main; "
        "end C4919dot;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (3) ACCEPT — control: `?.` member-call with a NON-nullable literal arg
// (`h?.IdB(42)`). Pre-fix: ACCEPT (the literal `42` is `int`, which is
// not nullable — the §7 gate never fires). Post-fix: ACCEPT (same).
// Pinning the row protects against a fix that over-corrects into a
// "reject any `?.` call" predicate.
BEZEL_TEST(nullcond_generic_nullable_arg_accept_4919,
           h_dot_IdB_literal_42_on_Holder_intQ_accepts_prints_42_control) {
    auto r = runAot(
        "module M4919lit; "
        "public class Holder4919lit<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T return v; end IdB; "
        "end Holder4919lit; "
        "public class C4919lit "
        "  public static method Main(): int "
        "    using var hh := new Holder4919lit<int?>(); "
        "    var h: Holder4919lit<int?>? := hh; "
        "    Console.WriteLine(h?.IdB(42) ?? -1); "
        "    return 0; "
        "  end Main; "
        "end C4919lit;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// (4) REJECT — control: `h?.IdB(x)` on `Holder<int>?` (T = int,
// NON-nullable receiver class arg) with `x: int?`. Both pre- and
// post-fix REJECT: the receiver substitution yields `v: int` (T is
// bound to `int`, not `int?`); the arg `x: int?` is §7-incompatible
// with that. This is the predicate-too-permissive tripwire — a fix
// that always admitted nullable args on `?.` would ACCEPTS here, and
// this row pins the correct rejection.
BEZEL_TEST(nullcond_generic_nullable_arg_accept_4919,
           h_dot_IdB_x_on_Holder_int_rejects_nullable_arg_control) {
    auto r = parseSemaAll(
        "module M4919reject; "
        "public class Holder4919reject<T> "
        "  public constructor() end constructor; "
        "  public method IdB(v: T): T return v; end IdB; "
        "end Holder4919reject; "
        "public class C4919reject "
        "  public static method T() "
        "    using var hh := new Holder4919reject<int>(); "
        "    var h: Holder4919reject<int>? := hh; "
        "    var x: int? := null; "
        "    _ := h?.IdB(x); "
        "  end T; "
        "end C4919reject;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
}
