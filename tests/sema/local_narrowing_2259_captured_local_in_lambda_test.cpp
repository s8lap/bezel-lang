// Spec-derived regression tests for issue #2259 — a flow-narrowing fact
// established on a LOCAL before a lambda would unsoundly ride INTO the lambda
// body, so a `s.Length` read inside the body type-checked against the
// pre-capture fact even though §27.5 capture is by REFERENCE.
//
// §27.5 (v0.236, "Closures"): "Modifications to a captured variable inside the
// lambda are visible outside, and vice versa." The "and vice versa" is the
// soundness hole: the enclosing scope may rebind a captured local's cell AFTER
// the closure's creation but BEFORE its invocation, invalidating a fact the
// lambda body already type-checked against. Probe-confirmed on origin/main
// (commit f360b6e) — the issue's exact repro SIGSEGV'd under generated-code
// AddressSanitizer: "SEGV on unknown address 0x000000000008 … in lam$0", the
// `.Length` load off a null receiver (the captured `s` was rebound to null by
// `s := null;` between closure creation and `f!()`).
//
// Root cause: commit 817a498 (#2113) scope-restored all three narrowing sets
// across a lambda body visit (closing the OUT-side leak — a body-established
// fact must not escape to the enclosing scope) and additionally dropped
// LOCAL-rooted `fieldNarrowed_` facts on the way IN (the captured root may be
// rebound between closure creation and invocation). But its comment argued the
// two LOCAL sets (`nullNarrowed_`/`narrowings_`) "need NO in-side drop" because
// §7.4's invalidation rules "have already removed anything unsound by the point
// of capture." That covers mutations BEFORE capture only; it misses the
// §27.5 "vice versa" rebind AFTER creation. ExprTypecheck_Exprs.cpp's lambda
// visitor (the save/restore block) is the fix site.
//
// Fix (v0.236, #2259): clear `nullNarrowed_` and `narrowings_` on the way INTO
// the lambda body — the direct analog of the field set's "drop all
// local-rooted" erase that immediately precedes it, and for the identical
// reason. Both sets are ENTIRELY local-rooted (LocalSymbol/ParameterSymbol; §7.4
// narrows no `this`/static through them — readonly fields go in fieldNarrowed_),
// so clearing the whole set is observably equivalent to dropping only the
// captured names' entries (the body cannot reference an uncaptured local, and
// the OUT-side restore preserves the enclosing facts). Re-narrow inside the
// body (`if x <> null then … end`) to recover a fact.
//
// Every REJECT below was a silent ACCEPT on origin/main (the captured local's
// pre-capture fact rode in); every ACCEPT control guards the other side — a
// soundness fix that widens rejection must not cost a legal program its
// narrowing.

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

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — nullNarrowed_: the issue's exact repro. `s` is narrowed non-null by
// the early-return guard BEFORE the lambda; the lambda captures `s` by
// reference (§27.5) and reads `s.Length` inside the body. Pre-fix the
// pre-capture null fact rode in and `s.Length` compiled; the trailing `s :=
// null; return f!();` then read null at invocation time → SIGSEGV. The fix
// drops the fact on the way IN, so the body sees `s: string?` and rejects.
// ---------------------------------------------------------------------------

// §7.4 / §27.5 (#2259): block body — the issue's repro form (a `do … end`
// body that reads the captured local).
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           captured_local_null_fact_dropped_in_block_body_spec_7_4_27_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    var f: (() -> int)? := null; "
        "    if s = null then return 0; end; "
        "    f := lambda(): int => do return s.Length; end; "
        "    s := null; "
        "    return f!(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4 / §27.5 (#2259): expression body — the same defect in the expression-
// body form (`lambda() => s.Length`). The body sees `s: string?` and rejects.
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           captured_local_null_fact_dropped_in_expr_body_spec_7_4_27_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    var f := lambda() => s.Length; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4 / §27.5 (#2259): a PARAMETER captured (not just a `var` local). The
// issue's repro captures the parameter `s`; parameters are mutable
// (assignable), so the same §27.5 "vice versa" rebind applies and the fact
// must drop. Covers the LocalSymbol vs ParameterSymbol axis of the local sets.
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           captured_parameter_null_fact_dropped_spec_7_4_27_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s <> null then "
        "      var f := lambda() => s.Length; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// REJECT — narrowings_: the §12.7 type-test fact rides in just the same. `b`
// is narrowed to `Derived` by `if b is Derived` BEFORE the lambda; the lambda
// captures `b` and dispatches `b.M()` (declared on `Derived` only). Pre-fix the
// type fact rode in and `b.M()` resolved against `Derived`; the enclosing
// `b := otherBase;` rebind makes that a wrong-typed dispatch at invocation.
// The fix reverts `b` to `Base` inside the body, so `b.M()` is rejected.
// ---------------------------------------------------------------------------

// §12.7 / §27.5 (#2259): a `is` type-narrowing on a captured local does not
// ride into the lambda body — `b.M()` is rejected (`M` is not on `Base`).
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           captured_local_type_fact_dropped_spec_12_7_27_5) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    if b is Derived then "
        "      var f := lambda(): int => do return b.M(); end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "no member `M` on type `Base`"));
}

// ---------------------------------------------------------------------------
// ACCEPT controls — the fix drops a fact only for a local the body captures;
// every legitimate shape must keep compiling.
// ---------------------------------------------------------------------------

// §7.4 (#2259 control): re-narrowing INSIDE the lambda body establishes a fresh
// fact valid inside the body — the in-side clear does not prevent that. This
// is the remedy the diagnostic names (guard with `if s <> null`).
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           accept_re_narrow_inside_lambda_body_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    var f := lambda(): int => do "
        "      if s = null then return 0; end; "
        "      return s.Length; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#2259 control): a NON-captured narrowed local used in the ENCLOSING
// scope AFTER the lambda is unaffected — the OUT-side restore brings back the
// saved fact set (the #2113 out-side leak fix is unchanged). `t` is narrowed
// before the lambda, not referenced inside the body, and `t.Length` after the
// lambda still compiles.
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           accept_noncaptured_local_after_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?, t: string?): int "
        "    if s = null then return 0; end; "
        "    if t = null then return 0; end; "
        "    var f := lambda(): int => do "
        "      if s = null then return 0; end; "
        "      return s.Length; "
        "    end; "
        "    return f() + t.Length; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#2259 control): a lambda that captures NO narrowed local leaves the
// enclosing facts untouched on both sides. The body references only its own
// parameter; the enclosing `s.Length` (after the early-return guard) compiles.
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           accept_lambda_capturing_nothing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    var f := lambda(n: int): int => n + 1; "
        "    return f(s.Length); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.7 (#2259 control): re-establishing the type narrowing INSIDE the body
// recovers the dispatch. The in-side clear of `narrowings_` does not prevent a
// fresh `is` test inside the body.
BEZEL_TEST(local_narrowing_2259_captured_local_in_lambda,
           accept_re_narrow_type_inside_lambda_body_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    var f := lambda(): int => do "
        "      if b is Derived then return b.M(); end; "
        "      return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
