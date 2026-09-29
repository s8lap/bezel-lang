// Regression tests for issue #4902 — §22.5/§22.6: an inheritance clause
// with named arguments against a base that declares two equal-arity
// constructors silently accepts the program without constructing the base.
//
// When two constructors share the same arity (e.g., both take 2 args),
// the arity scan finds `ambiguousArity = true` and sets `clauseCtor =
// nullptr`, then the block below (which runs only when `clauseCtor &&
// !ambiguousArity`) never executes, and gap (a)'s own scan (with the same
// guard) also never executes. The clause is silently discarded: the base
// subobject is never constructed.
//
// The fix adds a dedicated block that fires when `ambiguousArity` is true and
// the clause carries named arguments: it scans every candidate and the base's
// primary list; if none bind any name, it reports a specific diagnostic.
//
// spec §22.5 "Optional parameters and named arguments" ("the error is reported
// only where no candidate survives"; naming a parameter that no candidate
// declares "is a compile error at the call naming that parameter"), §22.6
// stage 1 (arity filtering and named-argument binding as one step), v0.307.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// `class B` with TWO equal-arity (2) constructors whose parameter names do
// not overlap — neither `(a: int, b: int)` nor `(c: string, d: int)` has a
// parameter named `zz` or `b`. The clause `B(zz := v, b := 9)` should be
// rejected.
std::string equalArityOverloadSet(const std::string& clauseArgs) {
    return "module M; "
           "class B "
           "  public n: int := 0; "
           "  public s: int := 0; "
           "  public constructor(a: int, b: int) "
           "    n := a; "
           "    s := b; "
           "  end constructor; "
           "  public constructor(c: string, d: int) "
           "    n := d; "
           "    s := 999; "
           "  end constructor; "
           "  public method Show() "
           "    Console.WriteLine($\"n={n} s={s}\"); "
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

// ---------------------------------------------------------------------------
// PIN — the exact repro from issue #4902: two equal-arity (2) ctors
// `(a: int, b: int)` and `(c: string, d: int)`, clause `B(zz := v, b := 9)`
// with names that belong to NO constructor of `B`. The `ambiguousArity` path
// should still reject the clause.
// ---------------------------------------------------------------------------
// per §22.5 / §22.6 (v0.307), fix #4902
BEZEL_TEST(inheritance_clause_equal_arity_named_args_4902,
           equal_arity_overload_unknown_names_rejected) {
    auto r = runAot(equalArityOverloadSet("zz := v, b := 9"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "supplies named argument(s) but no candidate in the overload "
           "set declares any parameter by those name(s)"));
}

// ---------------------------------------------------------------------------
// Control — a name that DOES bind one of the equal-arity constructors is not
// rejected. `b := 9` binds the `b` parameter of `(a: int, b: int)` and
// `a := v` binds the `a` parameter of the same constructor. The clause is
// valid (ignoring that `zz` is still unknown).
// ---------------------------------------------------------------------------
// per §22.5 / §22.6 (v0.307), fix #4902
BEZEL_TEST(inheritance_clause_equal_arity_named_args_4902,
           equal_arity_overload_named_clause_valid_is_accepted) {
    auto r = runAot(equalArityOverloadSet("a := v, b := 9"));
    // The clause binds `(a: int, b: int)` with args `(v, 9)` — valid.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=9\n"));
}

// ---------------------------------------------------------------------------
// Control — all-positional clause at ambiguous arity should still accept
// (silently, since §22.6 type-ranking picks the first match at codegen).
// ---------------------------------------------------------------------------
// per §22.5 / §22.6 (v0.307), fix #4902 — no-regression control
BEZEL_TEST(inheritance_clause_equal_arity_named_args_4902,
           equal_arity_overload_all_positional_still_accepted) {
    auto r = runAot(equalArityOverloadSet("v, 9"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
