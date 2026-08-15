// fix #3173 (§22.5 "Optional parameters and named arguments"; spec v0.254) —
// the §22.5 argument-to-formal binder (`ExprTC::bindArgsToParams`) is the ONE
// binder for ALL four formal-list sources: `ast::Param`, MethodSymbol,
// ConstructorSymbol, AND the positional-field list of a record / class
// primary-ctor (§9.1 / §22.10). The view (`sema::FormalsView`) is the
// abstraction over those sources, and the binder's five diagnostic strings
// (arity too few, arity too many, positional-after-named, unknown parameter
// name, parameter bound twice) are the ONE §22.5 surface — regardless of
// whether the callee is a method, a constructor, or a positional-field
// primary-ctor.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// Before #3173, the positional-field path in `ExprTypecheck_Calls.cpp`'s
// `checkPositionalNewArgsFromSymbols` was a SECOND COPY of the §22.5 rule
// (#3141). Five byte-identical diagnostic strings were held identical only
// by authors typing the same sentence, and the copy was structurally free
// to drift (e.g. a future "more user-friendly wording" change in the original
// would silently miss the copy). Every prior test in this area
// (e.g. `tests/codegen/local_primary_ctor_new_arg_check_3163_test.cpp`'s
// T1-T7, `tests/module/xmod_named_arg_imported_primary_ctor_3196_test.cpp`'s
// T1) drove the binder through that second copy. This test pins the
// UNIFICATION: same diagnostic strings via the unified binder.
//
// ── THE FIX (one-line summary) ─────────────────────────────────────────────
//
// `sema::FormalsView` gained a fourth producer — a
// `std::vector<const Symbol*>` (the positional-field list
// `recordPositionalFieldSymbols` already returns). `bindArgsToParams` already
// takes a `FormalsView`; the second copy in
// `checkPositionalNewArgsFromSymbols` becomes a one-line
// `bindArgsToParams(FormalsView::of(fields), ne.args, ne.span.begin, desc,
// /*emit=*/true)` call, followed by the unchanged per-argument §5.1 walk.
//
// ── ORACLES ────────────────────────────────────────────────────────────────
//
// T1-T5 pin the five §22.5 diagnostic strings through the positional-field
// path, with byte-identical wording to the `ast::Param` / MethodSymbol path
// — a coarse "is rejected" predicate would happily accept a neighbouring
// false reject, so the substring pins which rule fired. T6-T8 are positive
// (compile + run; exit-code checks) and pin that the unified binder's
// accepted result still lowers correctly.
//
// All rows go through runAot so the same driver the lane relies on is the
// one driving the assertion — the gate is sharded-vs-serial, NOT
// `--full`-vs-plain (#2633), and this test runs under both oracles.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run the program; return the exit code on a clean run, -999 if the compile
// was REJECTED (mirrors runJit's failWith semantics so the positive rows can
// distinguish "wrong answer" from "compile failed").
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program was REJECTED at compile/link. A coarse predicate —
// the negative rows also assert on the diagnostic substring so a neighbouring
// false reject cannot pass.
bool rejected(const std::string& prog) {
    auto r = runAot(prog);
    return r.diagnostics.hasErrors();
}

// Pin a SPECIFIC diagnostic substring so a coarse "rejected" check cannot
// accept a different reject (e.g. a syntax error in the test program itself
// masquerading as the rule under test). The substring is the EXACT wording
// `bindArgsToParams` emits in `src/sema/ExprTypecheck_Calls.cpp` — the
// positional-field path emits it through the unified binder (#3173).
bool rejectedWith(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (std::string(d.message).find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — §22.5 "arity too few" through the positional-field path.
//
// `new PBox(3)` for `class PBox(V: int, W: int)`: pre-#3163 silently
// produced a zero object; post-#3163 / post-#3173 the missing required
// parameter is reported by the SAME §22.5 wording the binder emits for an
// `ast::Param` list / MethodSymbol — i.e. the wording of the unified
// binder, not a per-source wording. The substring pin is what catches a
// regression that reverts the unification (#3173).
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           arity_too_flow_emits_unified_diagnostic_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3);"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    // The unified binder's wording — NOT a per-source variant — must fire.
    // A future fix that re-introduces a per-source wording on the
    // positional-field path would lose this substring and fail this row.
    BEZEL_EXPECT_TRUE(rejectedWith(src, "required parameter `W` is not "
                                         "supplied"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T2 — §22.5 "arity too many" through the positional-field path.
//
// `new PBox(1, 2, 3)` for `class PBox(V: int, W: int)`: the unified binder
// emits the "supplies N argument(s) but the parameter list has M" wording.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           arity_too_many_emits_unified_diagnostic_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(1, 2, 3);"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "supplies 3 argument(s) but the "
                                         "parameter list has 2"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T3 — §22.5 "positional-after-named" through the positional-field path.
//
// `new PBox(W := 4, 3)` for `class PBox(V: int, W: int)`: positional
// arguments come before named ones. The unified binder emits the same
// wording regardless of the source.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           positional_after_named_emits_unified_diagnostic_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(W := 4, 3);"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "a positional argument may not "
                                         "follow a named argument"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T4 — §22.5 "unknown parameter name" through the positional-field path.
//
// `new PBox(NOPE := 3)` for `class PBox(V: int, W: int)`: the named
// argument names an existing parameter — `NOPE` does not.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           unknown_named_param_emits_unified_diagnostic_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(NOPE := 3);"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "has no parameter named `NOPE`"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T5 — §22.5 "parameter bound twice" through the positional-field path.
//
// `new PBox(3, V := 4)` for `class PBox(V: int, W: int)`: `V` is bound
// positionally by `3` AND by name `V := 4`.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           double_bound_emits_unified_diagnostic_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3, V := 4);"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "parameter `V` is bound more than "
                                         "once"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T6 — POSITIVE happy path: the unified binder still lowers correctly
// through the positional-field path (was: pre-#3163 silent-zero object).
//
// `new PBox(3, 4)` for `class PBox(V: int, W: int)`: V = 3, W = 4.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           correct_arity_still_lowers_spec_22_10) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3, 4);"
        "  return b.Sum();"
        "end Main; end C;"), 7);
}

// ===========================================================================
// T7 — POSITIVE named-argument path: the unified binder handles
// `new PBox(W := 4, V := 3)` correctly (§22.5 + §22.10).
//
// This is the LOCAL twin of #3196's cross-module row — same shape, single
// module. Both run through `bindArgsToParams` over the positional-field view
// (#3173), and the side-table publication (`setNewExprPositionalBinding`)
// receives the binding the unified binder computed.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           named_arguments_lowers_through_unified_binder_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(W := 4, V := 3);"
        "  return b.Sum();"
        "end Main; end C;"), 7);
}

// ===========================================================================
// T8 — POSITIVE RECORD path: the unified binder works through the §9.1
// positional-field view for a RECORD too, where the field list has no
// per-slot defaults (A.3 `PositionalField` admits no default syntax).
//
// `new P(3, 4)` for `record P(V: int, W: int)`: V = 3, W = 4.
// ===========================================================================
BEZEL_TEST(positional_field_formals_view_3173,
           record_positional_field_binder_still_works_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public record P(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end P;"
        "class C public static method Main(): int"
        "  using var b := new P(3, 4);"
        "  return b.Sum();"
        "end Main; end C;"), 7);
}