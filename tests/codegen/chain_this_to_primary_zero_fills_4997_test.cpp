// Regression tests for issue 4997 — a convenience constructor's `this(...)`
// chain to its class's §22.10 primary list stores NOTHING when a same-arity
// sibling explicit constructor exists.
//
// WHAT §22.5 SAYS. "Each supplied argument binds exactly one parameter and
// each parameter is bound by at most one supplied argument". The §22.10 /
// §22.8 step 0 lowering site for a `this(...)` chain that binds the primary
// list is the same ONE store every other construction site reads through
// (`storePrimaryCtorPositionalArgs` over `SemaContext::positionalBinding`).
//
// WHAT THE COMPILER DID. `ExprTC::publishPrimaryCtorChainBinding`
// (ExprTypecheck_Calls.cpp) declined to publish a primary-parameter binding
// whenever ANY other non-static constructor of the same class had the SAME
// ARITY as the chain call. That is wrong on every §22.5 axis: a sibling
// whose parameters carry none of the call's NAMES (`(p: bool, q: bool)`
// does not bind `this(b := 2, a := 1)`) cannot be the chain target, so
// publishing the primary binding is exactly the right move. Codegen's
// `Emitter::emitChainCall` uses the same `ctorForArgs` binder — when no
// sibling §22.5-binds, `ctorForArgs` returns null and `emitChainCall`
// reaches its `!target` arm, which reads `SemaContext::positionalBinding`;
// with no published binding, `storePrimaryCtorPositionalArgs` declines to
// store (its `allPositional = false` named-args guard, or its no-binding
// / positional-only fallback), and the object stays calloc-zero.
//
// MEASURED ON pre-fix:
//   new C("x","y").S()   printed "0,0" (named args, no binding → store
//                              refused → calloc zero on every slot)
//   new C("x").S()       printed "0,0" (omitted `b`, `ctorForArgs` matched
//                              the (p: bool) sibling positionally, the
//                              sibling's `this(7)` chain again had no
//                              primary binding)
//
// HOW IT IS FIXED. The sibling filter now asks the §22.5 question: probe
// each OTHER non-static constructor with `bindArgsToParams` (emit=false)
// and decline to publish only when one of them BINDS the call. Same body
// every other construction site uses; the same `ctorForArgs` selector
// `emitChainCall` runs reaches the same verdict.
//
// WHICH ROWS ARE THE PINS. `q01_named_args_*` and `q02_omitted_defaulted_*
// fail on the pre-fix compiler (the issue's exact repros). The four
// controls pass on BOTH sides — they pin that the fix did not re-introduce
// the #3562 sibling-binds / primary-doesn't shape, that an arity-mismatch
// sibling is undisturbed, and that the same defect through a 3-arity
// sibling does not silently route through a binder that does not bind.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// THE PIN (issue body's own repro 1, q01). `this(b := 2, a := 1)` against
// `class C(a: int, b: int := 0)` with a same-arity sibling `(p: bool, q:
// bool)`. §22.5 binds `b := 2` to the `b` slot and `a := 1` to the `a`
// slot; the sibling has no parameter named `a` or `b`, so the primary is
// the only candidate. Pre-fix the store was refused on the named-args /
// no-binding shape and the object stayed calloc-zero.
//
// Asserts the printed VALUES, not just the exit status: a fix that
// suppresses the store silently and exits 0 prints "0,0" — exactly the
// pre-fix value — and is invisible to an exit-status oracle.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           q01_named_args_bind_to_primary_when_sibling_arity_matches_22_5) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string, q: string) this(b := 2, a := 1); "
        "  end constructor; "
        "  public constructor(p: bool, q: bool) this(7); end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\",\"y\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,2\n"));
}

// ---------------------------------------------------------------------------
// THE SECOND PIN (issue body's repro 2, q02). `this(a := 4)` against
// `class C(a: int, b: int := 0)` with a one-arity sibling `(p: bool)`.
// Pre-fix `ctorForArgs` matched the sibling positionally (one arg, one
// parameter), the chain call routed through it, and the sibling's
// `this(7)` chain again had no primary binding. Print "0,0" either way;
// the fix routes the call to the primary directly when no sibling binds.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           q02_omitted_defaulted_primary_param_takes_default_22_5) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string) this(a := 4); end constructor; "
        "  public constructor(p: bool) this(7); end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4,0\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — repro 1 WITHOUT the `(p: bool, q: bool)` sibling. With no
// sibling at all, the arity predicate never fired pre-fix either, so the
// control must pass on BOTH sides. It is here because a fix that inverted
// the gate (publishing only when a sibling exists, or unconditionally)
// would re-key a no-sibling class and break exactly this shape.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           control_no_sibling_still_publishes_primary_binding) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string, q: string) this(b := 2, a := 1); "
        "  end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\",\"y\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,2\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — repro 1 with a SIBLING OF DIFFERENT ARITY (`(p: bool)`, arity
// 1). The chain call has arity 2; the arity predicate never fired pre-fix
// either, so this passes on both sides. Pins that the fix did not
// accidentally widen the predicate to "any sibling exists".
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           control_sibling_of_different_arity_does_not_suppress_primary) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string, q: string) this(b := 2, a := 1); "
        "  end constructor; "
        "  public constructor(p: bool) this(7); end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\",\"y\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,2\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — repro 1 with a SIBLING OF HIGHER ARITY (`(p: bool, q: bool,
// r: bool)`, arity 3). The arity predicate never fired pre-fix either
// (the call's arity is 2, the sibling's is 3); this passes on both
// sides. Pins that the fix did not narrow the predicate to "exactly same
// arity" in the wrong direction (which would happen if the binder check
// were tested against the call's literal arity rather than the sibling's
// own parameter count).
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           control_sibling_of_higher_arity_does_not_suppress_primary) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string, q: string) this(b := 2, a := 1); "
        "  end constructor; "
        "  public constructor(p: bool, q: bool, r: bool) this(7); "
        "  end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\",\"y\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,2\n"));
}

// ---------------------------------------------------------------------------
// TRIPLE-OVERLOAD PIN — three siblings of varying arity (1, 2, 3). The
// chain call `this(b := 2, a := 1)` has arity 2; only the arity-2 sibling
// matches the pre-fix arity predicate. None of the three siblings NAME
// `a` or `b`, so the §22.5 binder check correctly determines that none
// of them binds and the primary is the right target.
//
// Pre-fix the arity-2 sibling alone suppresses the publish; print "0,0".
// Post-fix all three siblings fail the binder check; print "1,2". This
// is a stronger pin than q01 because it shows the fix's binder check
// being applied to ALL siblings, not just same-arity ones.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_this_to_primary_zero_fills_4997,
           triple_overload_no_sibling_binds_under_22_5_picks_primary) {
    auto r = runAot(
        "module M; "
        "class C(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(p: string, q: string) this(b := 2, a := 1); "
        "  end constructor; "
        "  public constructor(p: bool) this(9); end constructor; "
        "  public constructor(p: bool, q: bool) this(7); end constructor; "
        "  public constructor(p: bool, q: bool, r: bool) this(8); "
        "  end constructor; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(\"x\",\"y\").S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,2\n"));
}