// §22.5 / §22.6 (issue #5000, v0.307): `checkChainCallBinding`
// (ExprTypecheck_Calls.cpp) picking the wrong REPORT TARGET when no
// candidate §22.5-binds a `this(...)` / `base(...)` chain call, on a record
// whose primary parameter list shares the call's arity with an explicit
// constructor.
//
// WHAT THE DEFECT WAS. #4824 added a uniqueness test over `ctors` (every
// non-self, non-static explicit constructor) to decide whether the "no
// candidate binds" report can safely single out one candidate's own binding
// failure — but it never counted the primary parameter list as a candidate,
// even though `checkChainCallBinding` had already tried (and, to reach this
// path, failed) to bind the call against it. So a record whose primary list
// happens to share the call's arity with exactly one explicit constructor
// read as "one candidate", and the diagnostic named that constructor's
// formal that the call's argument list does not mention — even when the
// name it flagged (`Y`) is a REAL field the call actually supplied.
//
// MEASURED (issue #5000, on `404d7c51`):
//   record R(X: int, Y: int)
//     public constructor(s: string) this(Y := 2, Z := s.Length); end constructor;
//     public constructor(a: bool, b: bool) X := 1; Y := 1; end constructor;
//   end R;
// gave `e01_misattrib.bzl:3:33: \`this(...)\` constructor chain call in \`R\`
// has no parameter named \`Y\` (§22.5)` — naming `Y`, a field `X`/`Y` both
// declare, as though it did not exist. The call's actual defect is that `Z`
// names no candidate's parameter at all. The rejection itself is correct
// (§22.5 / §22.6: no candidate binds); only the wording was wrong.
//
// WHAT THE FIX DOES. The primary parameter list now counts as a candidate in
// the "single candidate" uniqueness test whenever its arity equals the
// call's, so two same-arity candidates (the primary list and the one
// explicit constructor) read as ambiguous and the generic "no ... constructor
// ... can bind these arguments" text (§12.8, §22.5, §22.6) is used instead of
// naming the wrong constructor's formals.
//
// FAIL-WITHOUT: with `src/sema/ExprTypecheck_Calls.cpp` restored to its
// pre-fix state, row 1 below fails (the diagnostic names `Y` rather than the
// generic text, and the `Y`-absent assertion also fails); with the fix, it
// passes.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

}  // namespace

// The issue's own repro (`e01`): `this(Y := 2, Z := s.Length)` — `Y` is a
// real primary field of `R`; `Z` names no candidate's parameter at all. The
// primary list `(X: int, Y: int)` and the sibling `(a: bool, b: bool)` share
// the call's arity (2), so the fix's uniqueness test must see two candidates
// and fall back to the generic message rather than naming `Y`.
BEZEL_TEST(ctor_chain_diag_target_5000,
           reports_generic_not_wrong_param_spec_22_5_22_6) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(s: string) "
        "    this(Y := 2, Z := s.Length); "
        "  end constructor; "
        "  public constructor(a: bool, b: bool) "
        "    X := 1; "
        "    Y := 1; "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abc\").X); "
        "    return 0; "
        "  end Main; "
        "end Program;");

    const char* corrected =
        "`this(...)` constructor chain call in `R`: no other constructor of "
        "`R` can bind these arguments — each candidate leaves a required "
        "parameter unbound, lacks a parameter the call names, or takes "
        "fewer arguments (§12.8, §22.5, §22.6)";
    const char* wrong = "no parameter named `Y`";

    if (!hasError(r, corrected) || hasError(r, wrong)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, corrected));
    BEZEL_EXPECT_FALSE(hasError(r, wrong));
    // The rejection itself must not change (§22.5 / §22.6: no candidate
    // binds `Z`).
    BEZEL_EXPECT_FALSE(r.ran);
}

// §22.5 / §12.8 (issue #5038, v0.307; follow-up to #5000): the same wrong-
// REPORT-TARGET defect, on a CLASS primary list with a DEFAULTED trailing
// parameter rather than a record's (always default-less) positional fields.
//
// WHAT THE DEFECT WAS. #5000's fix counted the primary parameter list as a
// same-arity candidate with `primaryArityMatches = fields.size() == n` —
// exact for a record (its grammar's `PositionalField`, Appendix A, admits no
// default), but a CLASS primary-ctor parameter list's `Param` DOES admit one
// (§22.10), so a call whose arity is below `fields.size()` because trailing
// primary parameters are DEFAULTED never counted the primary list at all.
//
// MEASURED (issue #5038, pre-#5038-fix, on `2f108d43`):
//   class C4(X: int, Y: int := 5)
//     public constructor(a: bool) end constructor;
//     public constructor(s: string) this(Y := 9); end constructor;
//   end C4;
// gave `` `this(...)` constructor chain call in `C4` has no parameter named
// `Y` (§22.5) `` — naming `Y`, a REAL primary field the call supplied, as
// though it did not exist. `X` has no default, so the call genuinely cannot
// bind (the primary list and the arity-1 sibling `constructor(a: bool)` are
// each unable to bind it — this is a `case (b)` "rule fires, compiler
// misattributes" defect, not an over-rejection: the rejection itself is
// correct and unchanged by the fix).
//
// WHAT THE FIX DOES. `checkChainCallBinding`'s `primaryArityMatches` is now
// a range test — `primaryRequiredCount <= n <= fields.size()` — that reads
// each slot's `hasDefault` off the same `FormalsView` the binder itself
// uses, so a CLASS primary list with a defaulted trailing parameter is
// counted as a same-arity candidate whenever the missing slots are all
// optional. A record's positional fields answer `hasDefault` false for
// every slot, so `primaryRequiredCount == fields.size()` there and the test
// degenerates back to the old exact match — the #5000 row above is
// unaffected.
//
// SEPARATELY CHECKED, NOT AN OVER-REJECTION: the task's proposed ACCEPT
// shape, `class C5(X: int := 1, Y: int := 5)` with a lone chaining
// `constructor(s: string) this(Y := 9); end constructor;`, compiles clean
// and runs printing `X + Y*1000 == 9001` (`X`'s own default, 1, plus the
// named `Y := 9`) — this path returns from `checkChainCallBinding` before
// reaching `primaryArityMatches` at all (the primary-list bind above it
// already succeeds, since `X` HAS a default here), so it was never affected
// by the pre-#5038-fix bug and needs no separate fix.
//
// FAIL-WITHOUT: with `src/sema/ExprTypecheck_Calls.cpp` restored to its
// pre-#5038-fix state (`2f108d43`), the row below fails (the diagnostic
// names `Y` rather than the generic text, and the `Y`-absent assertion also
// fails); rebuilt with the fix, it passes.
BEZEL_TEST(ctor_chain_diag_target_5038,
           defaulted_primary_param_reports_generic_not_wrong_field_spec_22_5) {
    auto r = runAot(
        "module M; "
        "class C4(X: int, Y: int := 5) "
        "  public constructor(a: bool) end constructor; "
        "  public constructor(s: string) this(Y := 9); end constructor; "
        "end C4; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");

    const char* corrected =
        "`this(...)` constructor chain call in `C4`: no other constructor of "
        "`C4` can bind these arguments — each candidate leaves a required "
        "parameter unbound, lacks a parameter the call names, or takes "
        "fewer arguments (§12.8, §22.5, §22.6)";
    // The genuinely correct report (`publishPrimaryCtorChainBinding`'s own
    // field-bind against the primary list: `X` is required and unbound) —
    // pinned so a future change cannot silently drop it while chasing this
    // row's other two assertions.
    const char* required =
        "`this(...)` chain to the primary constructor of `C4`: required "
        "parameter `X` is not supplied (§22.5)";
    const char* wrong = "no parameter named `Y`";

    if (!hasError(r, corrected) || !hasError(r, required) || hasError(r, wrong))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, corrected));
    BEZEL_EXPECT_TRUE(hasError(r, required));
    BEZEL_EXPECT_FALSE(hasError(r, wrong));
    // The rejection itself must not change (§22.5: `X` is required and the
    // call does not supply it).
    BEZEL_EXPECT_FALSE(r.ran);
}
