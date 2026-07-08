// Spec-derived tests for the `when true`/`when false` out-nullability
// postcondition (§22.4, v0.183, #1585; grammar A.5 OutNullPostcond).
//
// §22.4 "Nullability postcondition on `out` parameters": a bool-returning
// method may declare `when true`/`when false` after an `out`/`out owned`
// parameter of nullable type. The clause is:
//   * a compiler-checked postcondition — on every path that returns the stated
//     constant, the out parameter must be provably non-null (§7 narrowing
//     applies inside the callee);
//   * consumed by caller narrowing — in the region where the call's `bool`
//     result is known to equal the stated constant (the controlled `if`
//     branch, an `and`-conjunct), the recipient is treated as non-null, so the
//     otherwise-required `c!` (§7.4) becomes plain `c`;
//   * admitted ONLY on out/out-owned nullable parameters in bool-returning
//     methods; anywhere else is a compile error;
//   * part of the method contract — an override must repeat it (§12 modifier
//     match) — and does not create a distinct overload.
//
// The load-bearing narrowing is observed on a PRE-DECLARED nullable local
// passed as plain `out` (the recipient keeps its declared `T?` type). An
// inline `out var` recipient is already typed non-nullable in the current
// implementation (reference-type nullable-strip), so the narrowing is a no-op
// there and would not distinguish clause-present from clause-absent.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
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

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A pool class whose TryGet(out conn: Cn? when true) is spec-faithful, wrapped
// around an arbitrary caller body in App.Run.
std::string withPool(const std::string& runBody) {
    return std::string(
        "module M; "
        "class Cn public method Use(): int => 1; end Cn; "
        "class P "
        "  private _h: bool := true; "
        "  public method TryGet(out conn: Cn? when true): bool "
        "    if _h then conn := new Cn(); return true; end; "
        "    conn := null; return false; "
        "  end TryGet; "
        "end P; "
        "class App "
        "  public static method Run(): int ") +
        runBody +
        "  end Run; "
        "end App;";
}

} // namespace

// -------------------------------------------------------------------------
// Parsing + caller narrowing (the load-bearing user-facing effect).
// -------------------------------------------------------------------------

// §22.4: inside `if p.TryGet(out c) then`, the `when true` clause narrows the
// pre-declared nullable recipient `c` to non-null, so `c.Use()` is legal
// without `!`.
BEZEL_TEST(out_null_postcond_1585, when_true_narrows_then_branch_spec_22_4) {
    auto r = parseSemaAll(withPool(
        "    var p := new P(); "
        "    var c: Cn? := null; "
        "    if p.TryGet(out c) then return c.Use(); end; "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.4 / §7.4 (control): the SAME program WITHOUT the clause rejects `c.Use()`
// — proving the narrowing is what the postcondition establishes, not a default
// non-null typing of the recipient.
BEZEL_TEST(out_null_postcond_1585, without_clause_member_access_rejected_spec_7_4) {
    // withPool but with the clause stripped from TryGet's signature.
    std::string src =
        "module M; "
        "class Cn public method Use(): int => 1; end Cn; "
        "class P "
        "  private _h: bool := true; "
        "  public method TryGet(out conn: Cn?): bool "
        "    if _h then conn := new Cn(); return true; end; "
        "    conn := null; return false; "
        "  end TryGet; "
        "end P; "
        "class App "
        "  public static method Run(): int "
        "    var p := new P(); "
        "    var c: Cn? := null; "
        "    if p.TryGet(out c) then return c.Use(); end; "
        "    return 0; "
        "  end Run; "
        "end App;";
    auto r = parseSemaAll(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// §22.4: `when true` does NOT narrow the ELSE branch — on the false path the
// recipient may be null, so `c.Use()` there is rejected.
BEZEL_TEST(out_null_postcond_1585, when_true_does_not_narrow_else_spec_22_4) {
    auto r = parseSemaAll(withPool(
        "    var p := new P(); "
        "    var c: Cn? := null; "
        "    if p.TryGet(out c) then return 0; "
        "    else return c.Use(); end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// §22.4: `and`-conjunct — `flag and p.TryGet(out c)` narrows `c` across the
// combined true branch (§7.4 boolean composition).
BEZEL_TEST(out_null_postcond_1585, when_true_narrows_across_and_spec_22_4) {
    auto r = parseSemaAll(withPool(
        "    var p := new P(); "
        "    var c: Cn? := null; "
        "    var flag := true; "
        "    if flag and p.TryGet(out c) then return c.Use(); end; "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.4: `when false` narrows the recipient on the ELSE / false branch.
BEZEL_TEST(out_null_postcond_1585, when_false_narrows_else_branch_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn public method Use(): int => 1; end Cn; "
        "class P "
        "  private _h: bool := true; "
        "  public method TryMiss(out conn: Cn? when false): bool "
        "    if _h then conn := null; return true; end; "
        "    conn := new Cn(); return false; "
        "  end TryMiss; "
        "end P; "
        "class App "
        "  public static method Run(): int "
        "    var p := new P(); "
        "    var c: Cn? := null; "
        "    if p.TryMiss(out c) then return 0; "
        "    else return c.Use(); end; "
        "  end Run; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// -------------------------------------------------------------------------
// Callee-side postcondition check.
// -------------------------------------------------------------------------

// §22.4: a `return true;` path that leaves the `when true` out parameter null
// is a compile error in the declaring body.
BEZEL_TEST(out_null_postcond_1585, callee_return_true_with_null_rejected_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn end Cn; "
        "class P "
        "  public method TryGet(out conn: Cn? when true): bool "
        "    conn := null; return true; "
        "  end TryGet; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be provably non-null on every path that returns `true`"));
}

// §22.4: `return conn <> null;` discharges the `when true` clause — §7
// narrowing proves the parameter non-null on the true-return path.
BEZEL_TEST(out_null_postcond_1585, callee_return_test_discharges_clause_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn end Cn; "
        "class P "
        "  public method TryGet(out conn: Cn? when true): bool "
        "    conn := new Cn(); return conn <> null; "
        "  end TryGet; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.4: the `return false;` path (the OTHER constant) carries no obligation —
// it may leave the `when true` parameter null.
BEZEL_TEST(out_null_postcond_1585, callee_return_false_null_ok_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn end Cn; "
        "class P "
        "  private _h: bool := true; "
        "  public method TryGet(out conn: Cn? when true): bool "
        "    if _h then conn := new Cn(); return true; end; "
        "    conn := null; return false; "
        "  end TryGet; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// -------------------------------------------------------------------------
// Admittance: only on out/out-owned nullable params in bool methods.
// -------------------------------------------------------------------------

// §22.4: the clause on a plain (non-out) parameter is a compile error.
BEZEL_TEST(out_null_postcond_1585, reject_on_non_out_param_spec_22_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method A(x: C? when true): bool => true; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "admitted only on an `out` or `out owned` parameter"));
}

// §22.4: the clause in a non-bool-returning method is a compile error.
BEZEL_TEST(out_null_postcond_1585, reject_in_non_bool_method_spec_22_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method B(out x: C? when true): int "
        "    x := new C(); return 1; "
        "  end B; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "admitted only in a `bool`-returning method"));
}

// §22.4: the clause on a non-nullable out parameter type is a compile error.
BEZEL_TEST(out_null_postcond_1585, reject_on_non_nullable_out_param_spec_22_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method D(out x: C when true): bool "
        "    x := new C(); return true; "
        "  end D; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires a nullable parameter type"));
}

// -------------------------------------------------------------------------
// Override modifier-match (§12.1 / §22.4).
// -------------------------------------------------------------------------

// §22.4 / §12.1: an override that DROPS the clause does not match the base.
BEZEL_TEST(out_null_postcond_1585, override_dropping_clause_rejected_spec_12_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn end Cn; "
        "class Base "
        "  public virtual method TryGet(out x: Cn? when true): bool "
        "    x := new Cn(); return true; "
        "  end TryGet; "
        "end Base; "
        "class Derived : Base "
        "  public override method TryGet(out x: Cn?): bool "
        "    x := new Cn(); return true; "
        "  end TryGet; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`when` nullability postcondition"));
}

// §22.4 / §12.1: an override that REPEATS the clause matches — accepted.
BEZEL_TEST(out_null_postcond_1585, override_repeating_clause_accepted_spec_12_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn end Cn; "
        "class Base "
        "  public virtual method TryGet(out x: Cn? when true): bool "
        "    x := new Cn(); return true; "
        "  end TryGet; "
        "end Base; "
        "class Derived : Base "
        "  public override method TryGet(out x: Cn? when true): bool "
        "    x := new Cn(); return true; "
        "  end TryGet; "
        "end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
