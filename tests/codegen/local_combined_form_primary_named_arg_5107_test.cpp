// fix #5107 (§22.10 "primary constructors" / §22.5 "Optional parameters and
// named arguments" / §9.3 "Combined form"; spec v0.307) — a named argument
// to a local combined-form class's primary constructor builds a zero object.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// ExprTC::checkNewExprArgs (src/sema/ExprTypecheck_Calls.cpp) deferred
// LOCAL combined-form primary-constructor classes (hasExplicitCtor &&
// !isImported) WITHOUT publishing the §22.5 positional binding via
// `ctx_.setNewExprPositionalBinding`. Codegen's
// `storePrimaryCtorPositionalArgs` then hit its legacy fallback gate:
// with `binding == nullptr` and a named arg present, it returned early and
// left every field at calloc-zero.
//
// For:
//   class C(x: int, z: int)
//     public constructor(y: int) this(y, 0); end constructor;
//   end C;
// `new C(x := 7, z := 0)` resolved to the primary constructor (both named
// args bind primary params), but no binding was published, so codegen left
// fields `x` and `z` at zero. The explicit ctor body was never invoked —
// the call resolved to the primary, not the explicit ctor — so the object
// was silently wrong.
//
// ── THE FIX ───────────────────────────────────────────────────────────────
//
// Codegen's `Emitter::storePrimaryCtorPositionalArgs`
// (src/codegen/LlvmBackend_Exprs2.cpp:4604) was previously gated by a
// pre-#3196 fallback that returned early when `binding == nullptr` AND any
// call argument was named — leaving every primary field at calloc-zero. The
// LOCAL combined-form sema deferral at `checkNewExprArgs`'s
// `if (hasExplicitCtor) return;` arm (lines 17215-17248) bypassed the
// `checkPositionalNewArgsFromSymbols` call the primary-only local arm runs,
// so this helper received a null binding for `new C(x := 81, z := 0)`. The
// dc 2026-09-28 ruling ruled the previous lane's sema-side
// re-publication fix at `reportPrimaryBodyCtorAmbiguity` lines 16992-17003
// unreachable for the non-ambiguous case (the helper returns false on
// `cands.size() < 2` before the publish call), so the fix lives at THIS
// site instead: when `binding == nullptr` and a named arg is present, the
// helper builds the §22.5 binding on demand from the call's named arguments
// and `posFields`' declared-order parameter names (the same rule
// `bindArgsToParams` applies — positional precedes named, names match
// primary parameters, no duplicate binding, every required slot filled or
// defaulted). The position-i pairing fallback is retained for the
// all-positional, binding-unpublished shape, so an existing class whose
// positional `new` reach this helper through a different arm (e.g. the
// imported-class site's `ctx_.positionalBinding(ne)` null path) is
// unchanged.
//
// ── CLASS SHAPE RATIONALE ──────────────────────────────────────────────────
//
// The class uses primary arity 2 (x, z) and explicit-ctor arity 1 (y) so the
// four tests below each resolve UNAMBIGUOUSLY per §22.6:
//
//   * `new C(x := 81, z := 0)`  — both named args match primary params;
//                                 primary wins.
//   * `new C(42, 99)`           — arity 2; primary matches, explicit does not.
//   * `new C(y := 5)`           — only the explicit ctor's param `y` matches
//                                 the named arg; explicit wins, chains
//                                 `this(5, 0)` to primary (x := 5).
//   * `new C(7)`                — only the explicit ctor has arity 1;
//                                 explicit wins, chains `this(7, 0)` to
//                                 primary (x := 7).
//
// ── ORACLES ────────────────────────────────────────────────────────────────
//
// T1 uses a unique sentinel value (81 decimal) loop-amplified three times
// so one-shot runs reliably detect "field at calloc-zero" as a 0-sum
// (the pre-fix return is 0 because every primary field is left at its
// calloc-zero initial value, so the loop's three 0-additions yield 0).
// Three iterations × 81 = 243, which the process exit code carries
// verbatim (8-bit truncation only kicks in past 255).  The other three
// rows are single-shot.
//
// `public` on the explicit constructor is required by Parser_Decls.cpp:1520
// (§13 visibility).  `this(y, 0); end constructor;` is the chain-form body
// per Parser_Decls.cpp lines 1547-1558: "CtorChain (`base(args);` /
// `this(args);`) is folded into the body's first statement — sema recognises
// the pattern."  `public property X: int get => x;` exposes the primary
// parameter as required by §22.10 (primary-constructor parameters are NOT
// auto-promoted to public properties; an explicit property is the only way
// to read the field from outside the declaring class).
//
// ── COMBINED-FORM BODY (alternate form) ───────────────────────────────────
//
// The combined-form chain form uses `this(y, 0); end constructor;` — a
// statement-list body terminated by `end`.  The `{ }` block form used in
// some non-Bezel languages is NOT admitted here; Parser_Decls.cpp:1550-1558
// calls `parseStatement()` until `end`, and the `{` at the start of an
// empty block reads as a free-standing statement that the parser
// interprets as an expression statement needing a `;` — which is why the
// chain form is the canonical idiom and the block form was rejected on
// first compile.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run the program; return the exit code on a clean run, -999 if rejected.
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — §22.10 / §22.5 / §9.3: named args to primary of local combined-form
// class store the correct values.
//
// `new C(x := 81, z := 0)` on
//   class C(x: int, z: int)
//     public constructor(y: int) this(y, 0); end constructor;
//     public property X: int get => x;
//   end C;
// must store x = 81, z = 0, so c.X = 81.  Loop-amplified: sum of 3 copies
// of X must be 3 * 81 = 243.  Pre-fix, every field is left at calloc-zero,
// so the sum would be 0.  (The process exit code carries only the lower
// 8 bits of the int — a sum past 255 truncates and the test would silently
// invert, so the loop is sized to leave 243 < 256.)
// ===========================================================================
BEZEL_TEST(local_combined_form_primary_named_arg_5107,
           named_arg_to_primary_ctor_stores_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C(x: int, z: int)\n"
        "  public constructor(y: int) this(y, 0); end constructor;\n"
        "  public property X: int get => x; end;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        "  var sum: int := 0;\n"
        "  for var i in 0..2 do\n"
        "    using var c := new C(x := 81, z := 0);\n"
        "    sum := sum + c.X;\n"
        "  end for;\n"
        "  return sum;\n"   // expect 243; calloc-zero sum would be 0
        "end Main; end P;"), 243);
}

// ===========================================================================
// T2 — §22.10 / §22.5: positional args to primary still work (control).
//
// `new C(42, 99)` matches primary arity 2; primary runs, x = 42.  Pre-fix
// positional args were already published correctly (the bug is specific to
// the NAMED path), so this row is the alarm condition for a regression: if
// it goes red at FIX, the fix over-reached and broke the positional path.
// ===========================================================================
BEZEL_TEST(local_combined_form_primary_named_arg_5107,
           positional_arg_to_primary_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C(x: int, z: int)\n"
        "  public constructor(y: int) this(y, 0); end constructor;\n"
        "  public property X: int get => x; end;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(42, 99);\n"
        "  return c.X;\n"
        "end Main; end P;"), 42);
}

// ===========================================================================
// T3 — §22.10 / §22.5 / §9.3: a named arg that targets the EXPLICIT ctor's
// parameter chains `this(y, 0)` to the primary correctly.
//
// `new C(y := 5)` on the same class: only the explicit ctor's param `y`
// matches the named arg; the explicit ctor wins, chains `this(5, 0)` to the
// primary, so x = 5.  This pins the explicit-ctor → primary chain path.
// Per the fix commit message this path is reached through
// `publishPrimaryCtorChainBinding`'s own chain-call binding, so it should
// pass on both pre-fix and post-fix; if T3 fails at FIX the fix over-reached.
// ===========================================================================
BEZEL_TEST(local_combined_form_primary_named_arg_5107,
           named_arg_via_explicit_ctor_chains_correctly) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C(x: int, z: int)\n"
        "  public constructor(y: int) this(y, 0); end constructor;\n"
        "  public property X: int get => x; end;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(y := 5);\n"
        "  return c.X;\n"
        "end Main; end P;"), 5);
}

// ===========================================================================
// T4 — §22.10 / §22.5: the explicit ctor still wins when called
// positionally with a single arg matching its own parameter list (control).
//
// `new C(7)` on the same class: only the explicit ctor has arity 1; explicit
// wins, chains `this(7, 0)` to the primary, so x = 7.  Alarm condition is
// this row going red at FIX; if it does, the fix broke the explicit-ctor
// positional path.
// ===========================================================================
BEZEL_TEST(local_combined_form_primary_named_arg_5107,
           explicit_ctor_still_wins_with_matching_arity) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C(x: int, z: int)\n"
        "  public constructor(y: int) this(y, 0); end constructor;\n"
        "  public property X: int get => x; end;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(7);\n"
        "  return c.X;\n"
        "end Main; end P;"), 7);
}

