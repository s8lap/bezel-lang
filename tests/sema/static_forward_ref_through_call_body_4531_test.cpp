// fix #4531 / §22.13 (Static initialization order), v0.306: the SAME-TYPE
// forward-reference rule implemented only its DIRECT half. A `static readonly`
// initializer that reached a later-declared `static readonly` sibling through
// the body of a statically-resolvable call compiled, and then — since fix
// #4486 correctly stopped the runtime backstop raising on a same-type touch —
// silently observed the sibling's §7.9 default.
//
// The governing sentence, verbatim (§22.13, v0.306):
//
//   "Forward references between `static readonly` fields of the same type are
//   a compile error. Specifically: if the initializer expression for a `static
//   readonly` field F reads a `static readonly` field G of the same type
//   declared *after* F in the source — **directly, or transitively through the
//   bodies of statically-resolvable calls** (the same call set as the
//   cross-type dependency graph below — static methods and static property
//   getters reached by direct name within the compilation unit, §2.5; function
//   values, virtual dispatch, and code outside the compilation unit contribute
//   nothing) — the reference is rejected at compile time."
//
// and the escape the approximation actually grants, which the next sentence
// enumerates exhaustively:
//
//   "Like the cross-type graph, the transitive check is a deliberate
//   approximation, not a completeness guarantee: a same-type forward read
//   routed through a function value, virtual dispatch, or a module boundary
//   compiles, and at runtime observes the field's §7.9 default — for a
//   **non-nullable reference** static, the null watermark, which raises
//   `NullReferenceException` at the read in checked disposal mode (§22.7's
//   residual bound, applied to static storage) and is undefined behavior to
//   use unchecked."
//
// A direct static call within the compilation unit is in the CHECKED set, not
// among the three escapes — so rows 1-3 must be rejected, and row 4 (a lambda)
// must not be. §22.13 rule 2's runtime backstop is not an alternative route
// for any of them: it fires only when "that innermost initialization belongs
// to a different type — the T → U → T signature", and says so itself — "(A
// type's *own* initialization code observing its own later statics is governed
// by the forward-reference rule and its recorded residual above, not by this
// backstop.)"
//
// The fix lives in StaticInitCycles.cpp rather than in ExprTypecheck.cpp's
// direct arm for two reasons: that pass already owns §22.13's
// statically-resolvable call set (the section defines this clause by pointing
// at it — "the same call set as the cross-type dependency graph below"), and
// it runs after typecheck, which the helper needs — a helper is routinely
// declared AFTER the initializer that calls it, so during ExprTypecheck's own
// member-by-member pass its body carries no resolved symbols yet.
//
// FAIL-WITHOUT / PASS-WITH: measured on this worktree at 87313e75 by reverting
// only the src/sema/StaticInitCycles.cpp hunks — see the commit body for the
// counts. Rows 1-3 compiled clean pre-fix (row 1's program printed nothing and
// returned 1: G's §7.9 `int` default of 0, plus one); rows 4-6 pass on both
// sides, as controls must.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff some diagnostic is the §22.13 forward-reference rejection. Same
// predicate as static_autoprop_forward_ref_3774_test.cpp's, deliberately: the
// direct and transitive arms are one rule and must speak with one voice.
bool hasForwardRefError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("declared later in the same type") !=
                std::string::npos &&
            d.message.find("\xc2\xa7" "22.13") != std::string::npos)
            return true;
    return false;
}

// True iff some diagnostic names both members, per §22.13's "The error message
// names both fields and points to the textual ordering rule."
bool namesBoth(const bezel::test::JitResult& r, const char* f, const char* g) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(f) != std::string::npos &&
            d.message.find(g) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// Row 1 — REGRESSION, the issue's shape reduced to plain fields: F's
// initializer calls a same-type static METHOD whose body reads the
// later-declared G. `C4531A.Helper()` is "a static method reached by direct
// name within the compilation unit", so it is in the checked call set.
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t1_field_forward_ref_through_static_method_body_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H4531A "
        "  public static method Seed(): int => 7; "
        "end H4531A; "
        "public class C4531A "
        "  public static readonly F: int := C4531A.Helper(); "
        "  public static readonly G: int := H4531A.Seed(); "
        "  private static method Helper(): int => C4531A.G + 1; "
        "end C4531A; "
        "public class Main4531A "
        "  public static method Main(): int => C4531A.F; "
        "end Main4531A;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
    BEZEL_EXPECT_TRUE(namesBoth(r, "`F`", "`G`"));
}

// Row 2 — REGRESSION through the OTHER half of the call set: "static property
// getters reached by direct name". Reading `C4531B.Aux` runs its
// expression-bodied getter during F's initializer, and that body reads the
// later-declared G.
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t2_field_forward_ref_through_static_getter_body_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H4531B "
        "  public static method Seed(): int => 7; "
        "end H4531B; "
        "public class C4531B "
        "  public static readonly F: int := C4531B.Aux; "
        "  public static readonly G: int := H4531B.Seed(); "
        "  private static property Aux: int => C4531B.G + 1; "
        "end C4531B; "
        "public class Main4531B "
        "  public static method Main(): int => C4531B.F; "
        "end Main4531B;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
    BEZEL_EXPECT_TRUE(namesBoth(r, "`F`", "`G`"));
}

// Row 3 — REGRESSION at DEPTH TWO. §22.13 says "transitively", so one hop is
// not the rule: F -> One() -> Two() -> G. A one-level-only implementation
// passes rows 1 and 2 and fails here, which is why this row exists separately.
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t3_field_forward_ref_two_hops_through_call_bodies_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H4531C "
        "  public static method Seed(): int => 7; "
        "end H4531C; "
        "public class C4531C "
        "  public static readonly F: int := C4531C.One(); "
        "  public static readonly G: int := H4531C.Seed(); "
        "  private static method One(): int => C4531C.Two(); "
        "  private static method Two(): int => C4531C.G + 1; "
        "end C4531C; "
        "public class Main4531C "
        "  public static method Main(): int => C4531C.F; "
        "end Main4531C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
    BEZEL_EXPECT_TRUE(namesBoth(r, "`F`", "`G`"));
}

// Row 4 — CONTROL, the load-bearing one: the SAME forward read routed through
// a FUNCTION VALUE. §22.13 names function values as one of exactly three
// routes that "compile", and this is the shape fix #4486's runtime residual
// (static_same_type_forward_ref_residual_4486_test.cpp) is built on — so a
// transitive check that walked lambda bodies would silently delete that
// residual. Must NOT draw the forward-reference rejection.
//
// `ran` IS the oracle for that: a rejected program never links or executes, so
// reaching Main at all proves no error diagnostic was raised. (Asserting
// `!hasForwardRefError` on top of it would be the vacuous form
// scripts/check_vacuous_tests.py flags — the program does draw an unrelated
// §22.12 rule 4 *warning* about a static readonly field of function type, but
// no error of any kind.)
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t4_control_forward_ref_through_function_value_still_compiles_22_13) {
    auto r = runAot(
        "module M; "
        "public class H4531D "
        "  public static method Seed(): int => 7; "
        "end H4531D; "
        "public class C4531D "
        "  private static readonly _f: () -> int := lambda() => C4531D.G; "
        "  public static readonly F: int := _f(); "
        "  public static readonly G: int := H4531D.Seed(); "
        "end C4531D; "
        "public class Main4531D "
        "  public static method Main(): int => 0; "
        "end Main4531D;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 5 — CONTROL (over-reject guard): the same two-hop helper routing with
// the two fields in the CORRECT declaration order. §22.13 protects this
// explicitly — it is "what lets a field initializer call the type's own static
// helpers" — so the transitive walk must answer the textual-order question,
// not reject every same-type static read it finds in a followed body.
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t5_control_declaration_order_read_through_call_body_accepted_22_13) {
    auto r = runAot(
        "module M; "
        "public class H4531E "
        "  public static method Seed(): int => 7; "
        "end H4531E; "
        "public class C4531E "
        "  public static readonly G: int := H4531E.Seed(); "
        "  public static readonly F: int := C4531E.One(); "
        "  private static method One(): int => C4531E.Two(); "
        "  private static method Two(): int => C4531E.G + 1; "
        "end C4531E; "
        "public class Main4531E "
        "  public static method Main(): int => C4531E.F; "
        "end Main4531E;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);   // G = 7, F = G + 1
}

// Row 6 — CONTROL (termination): MUTUALLY RECURSIVE static helpers reached
// from a static readonly initializer. The interprocedural walk revisits Ping
// through Pong, so it terminates only by the recursion guard; without one the
// compiler hangs rather than diagnosing anything. The runtime recursion is
// bounded by the parameter, so `ran` and an exit code are real evidence the
// COMPILER terminated, not just that the program did.
BEZEL_TEST(sema_static_forward_ref_through_call_body_4531,
           t6_control_mutually_recursive_static_helpers_terminate_22_13) {
    auto r = runAot(
        "module M; "
        "public class R4531 "
        "  public static readonly V: int := Ping(0); "
        "  private static method Ping(n: int): int => "
        "    (if n > 2 then 7 else Pong(n + 1)); "
        "  private static method Pong(n: int): int => Ping(n + 1); "
        "end R4531; "
        "public class Main4531F "
        "  public static method Main(): int => R4531.V; "
        "end Main4531F;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
