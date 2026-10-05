// Regression tests for issue #4879 — §22.5/§22.6/§22.10: a §22.10
// inheritance clause that names its arguments against an OVERLOAD SET
// where the exact-arity scan finds a non-binding candidate but a sibling
// CAN bind (via defaults) was silently accepted with the base
// subobject ENTIRELY UNCONSTRUCTED, with zero diagnostics.
//
// The exact-arity scan in `ExprTC::visitTopDecl` finds exactly one
// constructor at the clause's arity. `bindArgsToParams` then fails because
// the clause's names don't match that constructor's parameter names. The
// gap (a) scan (which checks every sibling) found that `(x, y, z := 0)`
// CAN bind, so anyBinds = true and no error was emitted. But the binding
// was never published: clauseCtor stayed as the non-binding `(a, b)`
// candidate and cb stayed as !ok, so setInheritanceClauseCtorBinding was
// never called. Codegen then emitted no base-ctor call and the base
// fields stayed at their `:= 0` defaults — a silent wrong value.
//
// The fix: in gap (a), when a sibling CAN bind, update clauseCtor to
// that sibling and cb to its binding result, and publish the binding
// via setInheritanceClauseCtorBinding. The binding sibling's constructor
// is then correctly called in codegen.
//
// spec §22.5 "Optional parameters and named arguments" ("the error is
// reported only where no candidate survives"), §22.6 stage 1 (arity
// filtering and named-argument binding as one step), §22.10 "Primary
// constructors" ("the base call appears in the inheritance clause"),
// v0.307.
//
// MEASURED ON b83afbe3 and 7dd479cf (pre-fix): `class D(v: int) :
// B(x := v, y := 9)` over `class B` with constructors `(a: int, b: int)`
// and `(x: int, y: int := 0, z: int := 0)` compiled at exit 0 with NO
// diagnostic and printed `n=0 s=0` — the base's field initializers,
// because the base constructor NEVER RAN. `n=40 s=90` is correct: the
// clause binds `(x: int, y: int := 0, z: int := 0)` which sets n=40
// and s=90.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// `class B` with two constructors of DIFFERENT arity: `(a: int, b: int)`
// (arity 2) and `(x: int, y: int := 0, z: int := 0)` (arity 3). The
// clause `: B(x := v, y := 9)` supplies 2 args at named form. The exact-
// arity scan finds only the first constructor (arity 2), whose parameters
// are named `a` and `b` — not `x` and `y`. The fix reports the binder's
// specific error for the sole candidate's mismatch.
std::string baseWithTwoDifferentArityCtors() {
    return "module M; "
           "class B "
           "  public n: int := 0; "
           "  public s: int := 0; "
           "  public constructor(a: int, b: int) "
           "    n := a; "
           "    s := b; "
           "  end constructor; "
           "  public constructor(x: int, y: int := 0, z: int := 0) "
           "    n := x * 10; "
           "    s := y * 10; "
           "  end constructor; "
           "  public method Show() "
           "    Console.WriteLine($\"n={n} s={s}\"); "
           "  end Show; "
           "end B; "
           "class D(v: int) : B(x := v, y := 9) "
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

// ---------------------------------------------------------------------------
// PIN — the issue body's own repro. The clause `: B(x := v, y := 9)` supplies
// two named args. The exact-arity scan finds `(a: int, b: int)` (arity 2)
// but those params are named `a` and `b`, not `x` and `y`, so the binder
// declines. gap (a) then checks every sibling and finds that
// `(x: int, y: int := 0, z: int := 0)` CAN bind the clause names (with
// `z` taking its default). The fix publishes this binding: the sibling
// constructor is called, producing `n=40 s=90`.
//
// Pre-fix (b83afbe3, 7dd479cf): compiled at exit 0 with NO diagnostic,
// printed `n=0 s=0` — the base's field initializers, because the base was
// NEVER constructed.
//
// Post-fix: accepted, the binding sibling is called, `n=40 s=90`.
// ---------------------------------------------------------------------------
// per §22.5 / §22.6 / §22.10, issue #4879
BEZEL_TEST(inheritance_clause_named_arg_sole_candidate_mismatch_4879,
           named_clause_finds_binding_sibling_is_accepted) {
    auto r = runAot(baseWithTwoDifferentArityCtors());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=40 s=90\n"));
}
