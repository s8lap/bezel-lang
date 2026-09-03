// fix #3756 (§27.4 / §27.1, v0.286): an unannotated lambda parameter with
// NO target-type context anywhere compiled cleanly and silently computed the
// WRONG runtime value instead of being rejected.
//
// Spec §27.4 "Parameter type inference", verbatim: "When the lambda's target
// type is known from context, parameter type annotations may be omitted...
// In top-level contexts with no target type (`var f := lambda(x) => x * 2;`),
// parameter types must be specified." The `owned`-MODIFIER analogue lives in
// §27.1's "Lambda parameter modifiers" subsection, verbatim: "When the
// target type is known from context, the lambda's parameter modifiers may
// be omitted in source... When inferring is not possible (a `var`
// introduction with no target context), the modifier must be written
// explicitly to produce a function value of the modified type."
//
// Issue #3756's exact repro: `var h := lambda(owned c): int => c.V();` —
// the parameter `c` carries the `owned` MODIFIER but no TYPE annotation, and
// `h` is a `var` with no target-type ascription — compiled (rc=0) and
// `h(move p)` printed `0` where the correct answer is `5`. The root cause
// (ExprTypecheck_Exprs.cpp's lambda-literal parameter-typing arm,
// `visitChildren`'s `LambdaExpr` case): when NONE of the three type-sourcing
// paths applied (an explicit `: Type` annotation, the §24.3 event-handler /
// LINQ single-param context, or fix #829's positional
// `pendingLambdaParamTypes_` thread), the code fell through silently —
// `ParameterSymbol::declType` / `resolvedDeclType` stayed null, and so did
// the lambda's own function-type parameter slot — with no diagnostic at all.
//
// The fix adds a fourth, terminal branch: if none of the first three named a
// type AND the lambda is not a direct call-/constructor-argument (which has
// an unnamed-but-real target-type context of its own, tracked by the new
// `pendingLambdaArgContext_` — see the accept-side tests below), reject with
// a located §27.4 diagnostic.
//
// This file also pins the ACCEPT side, per LANE_RULES §6b's enumeration
// requirement: a typed local's function-type context, a method-argument
// position (including one resolved only by call-site GENERIC
// monomorphization, which the plain `pendingLambdaParamTypes_` thread does
// NOT cover per fix #829/#505's own deliberate exclusion), a constructor
// argument (which has no `pendingLambdaParamTypes_` thread at all), and a
// `var` with an explicit type annotation but no other context — none of
// these must regress into the new rejection.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Finds the FIRST diagnostic whose message contains `needle`, or nullptr.
const bezel::Diagnostic* findDiag(const bezel::test::JitResult& r,
                                   const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return &d;
    return nullptr;
}

// The diagnostic must point at the lambda, never at `0:0`.
//
// It cannot be checked through `d.location` (measured): on a compile REJECT
// the AOT harness synthesizes exactly ONE diagnostic whose `location` is a
// default-constructed `SourceLocation` (0:0) and whose `message` is the
// WHOLE captured stderr, `<file>:<line>:<col>: <text>`. The rendered prefix
// inside the message is the real location and the one the user sees, so
// that is what this reads: both numbers present and non-zero immediately
// before `needle`. Mirrors `locatedAt` in
// tests/sema/stdlib_v0284_removals_3559_test.cpp (same measured harness
// behavior, same idiom).
bool locatedAt(const bezel::test::JitResult& r, const std::string& needle) {
    const std::string tail = ": " + needle;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        for (std::size_t at = m.find(tail); at != std::string::npos;
             at = m.find(tail, at + 1)) {
            std::size_t p = at;
            auto digitsBack = [&m](std::size_t& q) -> long {
                const std::size_t end = q;
                while (q > 0 && m[q - 1] >= '0' && m[q - 1] <= '9') --q;
                if (q == end) return -1;
                return std::stol(m.substr(q, end - q));
            };
            const long col = digitsBack(p);
            if (col <= 0 || p == 0 || m[p - 1] != ':') continue;
            --p;
            if (digitsBack(p) > 0) return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — no target-type context anywhere.
// ---------------------------------------------------------------------------

// #3756's own issue-body repro, verbatim (modulo module-boilerplate
// compression to one line, matching this suite's style): `owned`-modifier
// parameter, no type annotation, `var` with no target context. Pre-fix this
// compiled (rc=0) and printed `0`; the fix rejects it — assert the located
// §27.4 diagnostic TEXT (not a coarse `hasErrors()` bit — LANE_RULES §3), and
// that the location is NOT the `0:0` sentinel (a diagnostic anchored on the
// lambda expression's own span, `lam->span.begin`, mirroring the established
// idiom immediately above it in the same function for the async-modifier
// diagnostic, since `ast::Param::loc` is never stamped by the parser for the
// plain-identifier lambda-parameter form — only for member-shaped params).
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           owned_modifier_no_annotation_no_context_rejected_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Take(p: owned R): int "
        "    var h := lambda(owned c): int => c.V(); "
        "    return h(move p); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    const auto* d = findDiag(r, "needs a type annotation");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`c`") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("\xC2\xA7""27.4") != std::string::npos);
    }
    // Not the 0:0 sentinel — LANE_RULES §3 (see `locatedAt`'s comment for
    // why this is checked via the rendered message prefix, not `d->location`).
    BEZEL_EXPECT_TRUE(locatedAt(r, "parameter `c` needs a type annotation"));
}

// Same shape with NO modifier at all (neither `owned` nor a type, no
// context) — issue #3756 asked whether the no-modifier untyped twin behaves
// the same way "today"; measured on this tree it ALSO silently defaulted
// (same wrong-value symptom), so the fix's reject condition does not key on
// the modifier at all — any missing-type/no-context parameter is rejected.
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           no_modifier_no_annotation_no_context_rejected_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Take(p: owned R): int "
        "    var h := lambda(c): int => c.V(); "
        "    return h(move p); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(findDiag(r, "needs a type annotation") != nullptr);
}

// §27.4 does not admit bidirectional inference from the body's usage — a
// lambda whose body performs int arithmetic on the untyped parameter is
// still rejected with no target type, exactly as the record-typed repro is.
// This guards against a narrower fix that only rejects when the body's
// member/operator resolution itself fails (it must reject unconditionally
// on "no context", independent of what the body does with the parameter).
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           arithmetic_body_no_context_still_rejected_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var h := lambda(c) => c + 1; "
        "    Console.WriteLine(h(5).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(findDiag(r, "needs a type annotation") != nullptr);
}

// ---------------------------------------------------------------------------
// ACCEPT — a target-type context exists; the fix must not false-reject it.
// ---------------------------------------------------------------------------

// The ANNOTATED twin from #3756's own issue body: an explicit `: R` type
// annotation on the parameter needs no context at all. Runs and prints the
// correct value (5), exactly as the issue states.
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           explicit_type_annotation_accepts_runs_5) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Take(p: owned R): int "
        "    var h := lambda(owned c: R): int => c.V(); "
        "    return h(move p); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// §27.4 / §6b: a TYPED LOCAL whose declared type is a function type supplies
// the target context (fix #829's `pendingLambdaParamTypes_` thread from
// `ld->type`) — the modifier-carrying, type-omitting parameter is accepted
// and the closure computes the correct value.
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           typed_local_function_type_context_accepts_runs_5) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Take(p: owned R): int "
        "    var h: (owned R) -> int := lambda(owned c) => c.V(); "
        "    return h(move p); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// §27.4 / §6b: a METHOD-ARGUMENT position is a target-type context even
// when the callee's OWN generic type parameter is what the slot is written
// in terms of (`Apply<T>(v: T, f: (T) -> int)`), a case fix #829/#505
// DELIBERATELY excludes from `pendingLambdaParamTypes_` threading (the
// comment: "the existing generic-inference path types the lambda param
// correctly, and threading the bare `T` here would override it"). Measured:
// sema leaves the parameter's `declType`/`resolvedDeclType` null even
// post-fix (this is exactly why the new `pendingLambdaArgContext_` thread is
// a SEPARATE, weaker signal — "a context exists" — from the three
// type-naming threads), yet the call-argument position is a genuine
// context per §27.4's rule and must not be rejected; the program still runs
// and computes the correct value (this is the generic_probe3 shape from the
// fix's own investigation, disambiguating which class's `V()` is called by
// the argument's ACTUAL runtime type — R prints `5`, not garbage).
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           generic_method_own_type_param_call_arg_accepts_runs_5) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Apply<T>(v: T, f: (T) -> int): int "
        "    return f(v); "
        "  end Apply; "
        "  public static method Main(): int "
        "    Console.WriteLine(Apply(new R(), lambda(x) => x.V()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// §27.4 / §6b: a CONSTRUCTOR-argument position is a target-type context too,
// even though `NewExpr` argument visiting has NO `pendingLambdaParamTypes_`
// seeding path at all (unlike the `CallExpr` path's fix #829 machinery) —
// this is the other half of why `pendingLambdaArgContext_` is set
// independently in the `NewExpr` arm. `scoped` marks the function-typed
// constructor parameter per §28.14 (a plain function-typed parameter may not
// be stored in a field).
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           constructor_argument_accepts_runs_5) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class Foo "
        "  private result: int := 0; "
        "  public constructor(g: (R) -> int) result := g(new R()); end constructor; "
        "  public method Result(): int return this.result; end Result; "
        "end Foo; "
        "class C "
        "  public static method Main(): int "
        "    var foo := new Foo(lambda(x) => x.V()); "
        "    Console.WriteLine(foo.Result().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// §27.4 / §6b: a TYPE-only annotation (no `owned` modifier — the parameter
// is a plain borrow) on a `var` with no other context needs no target-type
// thread at all; the explicit `: R` is itself sufficient (the `p.type`
// branch, unconditional on context). Guards that the new reject does not
// fire on a parameter that already carries an annotation.
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           type_annotation_no_modifier_no_context_accepts_runs_5) {
    auto r = runAot(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "end R; "
        "class C "
        "  public static method Main(): int "
        "    var h := lambda(c: R) => c.V(); "
        "    Console.WriteLine(h(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// §24.3 / stdlib §5, regression guard: a LINQ `Select` lambda's single
// untyped parameter is still typed from the source element (the
// `eventParamType` thread, first in priority order) — unaffected by the new
// terminal branch. Sum of [1,2,3] doubled = 12.
BEZEL_TEST(lambda_param_no_target_type_reject_3756,
           linq_select_single_param_still_accepts_unaffected) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var arr: Array<int> := [1, 2, 3]; "
        "    var doubled := arr.Select(lambda(x) => x * 2); "
        "    var sum := 0; "
        "    for var v in doubled do sum := sum + v; end for; "
        "    Console.WriteLine(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("12") != std::string::npos);
}
