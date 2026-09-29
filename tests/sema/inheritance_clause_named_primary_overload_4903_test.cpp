// Regression tests for issue #4903 — §22.5/§22.6/§22.10: a NAMED §22.10
// inheritance clause against a base that declares BOTH a primary parameter
// list and explicit body constructors, where the clause's names bind only the
// PRIMARY list.
//
// `ExprTC::visitTopDecl` chose between the base's primary list and its
// explicit constructors by ARITY alone (`explicitCtorOfThisArity`): any body
// constructor with as many parameters as the clause has arguments sent the
// clause to the explicit arm, whose binder declined (the names belong to no
// explicit constructor), so nothing was published for either formal list.
// With an overload set codegen's case (a) then paired the clause arguments
// with the primary slots in TEXTUAL order — a silent wrong value; with a sole
// same-arity body constructor the explicit arm's binder rejected the valid
// clause outright.
//
// spec §22.5 "Optional parameters and named arguments" ("naming an argument
// changes which parameter it binds to"; the binding reaches "every
// construction site"), §22.6 stage 1 (arity filtering AND named-argument
// binding), §22.10 "Primary constructors" (the clause "replaces an explicit
// base(...) call"), §11.1 (body constructors are declared in addition to the
// primary and chain to it), v0.307.
//
// MEASURED pre-fix on ad779ad0 + the #4791/#4834 batch-2 commits:
//   row 1 printed `p=4 q=9` (correct `p=9 q=4`), exit 0, no diagnostic;
//   row 2 was rejected: "inheritance-clause base call to a constructor of `B`
//     has no parameter named `q` (§22.5)".

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

std::string program4903(const std::string& bodyCtors,
                        const std::string& clauseArgs) {
    return "module M; "
           "class B(p: int, q: int) " +
           bodyCtors +
           "  public method Show() "
           "    Console.WriteLine($\"p={p} q={q}\"); "
           "  end Show; "
           "end B; "
           "class D(v: int) : B(" +
           clauseArgs +
           ") "
           "  public method Go() Show(); end Go; "
           "end D; "
           "class C "
           "  public static method Main(): int "
           "    using var d := new D(4); "
           "    d.Go(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// per §22.5 / §22.6 / §22.10 (v0.307), fix #4903 — the issue's repro shape
// (with a body constructor whose parameter list differs from the primary's,
// so the program is well-formed independent of the #4839 ruling).
BEZEL_TEST(inheritance_clause_named_primary_overload_4903,
           named_clause_binds_primary_beside_same_arity_overload_set) {
    auto r = runAot(program4903(
        "  public constructor(a: int, b: string) this(a, 0); "
        "  end constructor; "
        "  public constructor(a: int) this(a, 0); end constructor; ",
        "q := v, p := 9"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("p=9 q=4\n"));
}

// per §22.5 / §22.6 / §22.10 (v0.307), fix #4903 — a SOLE same-arity body
// constructor must not shadow the primary: the clause is valid and binds it.
BEZEL_TEST(inheritance_clause_named_primary_overload_4903,
           named_clause_binds_primary_beside_sole_same_arity_ctor) {
    auto r = runAot(program4903(
        "  public constructor(a: int, b: string) this(a, 0); "
        "  end constructor; ",
        "q := v, p := 9"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("p=9 q=4\n"));
}

// per §22.5 / §5.1 / §22.10 (v0.307), fix #4903 — a same-arity sibling whose
// types would ACCEPT the arguments positionally (`int` widens to `long`) must
// not be called with them: the names bind the primary.
BEZEL_TEST(inheritance_clause_named_primary_overload_4903,
           named_clause_binds_primary_beside_type_compatible_sibling) {
    auto r = runAot(program4903(
        "  public constructor(a: int, b: long) this(a, 0); "
        "  end constructor; ",
        "q := v, p := 9"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("p=9 q=4\n"));
}

// Control (unchanged disposition): a named clause whose names bind an
// EXPLICIT body constructor still calls that constructor.
BEZEL_TEST(inheritance_clause_named_primary_overload_4903,
           control_named_clause_binding_explicit_ctor_calls_it) {
    auto r = runAot(program4903(
        "  public constructor(a: int, b: string) this(a * 100, b.Length); "
        "  end constructor; "
        "  public constructor(a: int) this(a, 0); end constructor; ",
        "b := \"xyz\", a := v"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("p=400 q=3\n"));
}

// Control (unchanged disposition): a name no candidate declares is still the
// §22.5 compile error (fix #4878).
BEZEL_TEST(inheritance_clause_named_primary_overload_4903,
           control_unknown_name_still_rejected) {
    auto r = runAot(program4903(
        "  public constructor(a: int, b: string) this(a, 0); "
        "  end constructor; "
        "  public constructor(a: int) this(a, 0); end constructor; ",
        "zz := v, p := 9"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no parameter named `zz`"));
    BEZEL_EXPECT_FALSE(r.ran);
}
