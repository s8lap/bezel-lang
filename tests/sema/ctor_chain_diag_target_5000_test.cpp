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
