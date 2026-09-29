// Spec-derived tests for issue #2584 — a derived ctor that READS its own
// `ref` param inside a `base(...)` chain-call ARGUMENT EXPRESSION against a
// PRIMARY-CTOR base (`class Base(x: int)`, positional fields) emitted malformed
// IR (`add ptr, i32`) that LLVM's `verifyModule` rejects.
//
// Root cause: `Emitter::prePopulateBasePositionalFields`
// (LlvmBackend_Exprs2.cpp ~2390) is a pre-pass at the `new`-expression site
// that temporarily binds the ctor's params to evaluate the `base(...)`
// chain-call arg expressions and store them into the primary-ctor base's
// positional fields. It bound EVERY param (including `ref`/`out`) into
// `paramSlots_`. At this call site the ctor args follow the CG50 by-ref ABI —
// a `ref`/`out` param's arg is the caller's storage ADDRESS (a `ptr`) — so
// storing that address in a `paramSlots_` alloca and reading the param
// (`base(n + 1)` -> `n`) loads the POINTER itself, and `n + 1` emits
// `add ptr, i32`. `verifyModule` aborts the build.
//
// This is a DISTINCT site from #2283 (ctor BODY emission) and #2579 (the
// `emitChainCall` arg loop): neither covers the `prePopulateBasePositionalFields`
// temporary-bind. It requires a PRIMARY-CTOR base (positional fields) for this
// pre-pass to run; a regular base ctor does not hit it.
//
// §22.3 (v0.236), `ref` and `out` argument locations:
//   "A `ref` argument naming a mutable local or writable field is a true
//    alias: callee writes are visible through the caller's location
//    immediately …"
// §22.3 puts NO restriction on a ctor reading its own `ref` param in a
// chain-call arg expression, and sema admits it, so a §22.3-conformant program
// was wrongly rejected.
//
// The fix mirrors #2283's inline-ctor path and `bindParameters`: when the
// ctor args follow the by-ref ABI (the top-level `new`-expression call), a
// `ref`/`out` param's ADDRESS is registered in `refParams_` (save/restore
// around the temporary evaluation) so the chain-call arg expression
// dereferences through it — not via a bare `paramSlots_` load. The recursion
// passes lowered VALUES (ctorArgsByRefAbi=false) and keeps `paramSlots_`.
//
// Pre-fix behavior (mechanically confirmed on origin/main, commit f360b6e):
// every ref-param-reading case below failed with `run() == -999` —
// `verifyModule` aborted the build before the program could run. The value-
// param control (#5) passed both ways.
//
// `ref_out_chain_call_2579_test.cpp` covers the CHAIN call itself
// (`base(ref n)` — the arg is the ref param) against a NON-primary-ctor base;
// these tests cover the ARG EXPRESSION READING the ctor's own ref param
// (`base(n + 1)`) against a PRIMARY-CTOR base.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) {
        // Surface diagnostics so a regression is not silently -999.
        std::fprintf(stderr,
                     "DIAG (no .ran) for program of length %zu: %s\n",
                     prog.size(),
                     r.diagnostics.hasErrors() ? "has errors" : "no errors");
        if (r.diagnostics.hasErrors()) {
            for (const auto& d : r.diagnostics.diagnostics())
                std::fprintf(stderr, "  msg: %s\n", d.message.c_str());
        }
    }
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// =============================================================================
// The cardinal fix (issue #2584 repro): `base(n + 1)` where `n` is the derived
// ctor's own `ref` param, against a primary-ctor base `Base(x: int)`. The chain
// arg expression READS the ref param. Pre-fix: `add ptr, i32` malformed IR,
// run()==-999. Post-fix: the base's positional field x gets n+1 = 10+1 = 11.
// =============================================================================

// §22.3 / fix #2584: the issue's exact repro — `base(n + 1)` reads the derived
// ctor's own ref param n (which aliases the caller's x=10) and passes n+1=11
// to the primary-ctor base's positional field. Per spec §22.10 (v0.307), the
// base's primary-ctor param `x` is NOT auto-promoted to a public property
// after #4868, so the test must declare the property explicitly and read it
// through the declared accessor (per spec §11 / §22.10 idiom, doc line 1861).
// Pre-fix: d.x compiled via the public-stamped posParam surface defect. Post-fix
// (both #4868 and post-migration): d.X = 11.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_ref_arg_reads_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base(x: int) "
        "  public readonly property X: int := x; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(n + 1); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x); "
        "    return d.X; "
        "  end Main; "
        "end C;"), 11);
}

// §22.3 / fix #2584: a BARE read of the ref param — `base(n)` (no arithmetic).
// The primary-ctor base's field gets n = 10. Per spec §22.10 (v0.307, line
// 1861) the base's primary-ctor param `x` requires an explicit `public readonly
// property X` to be readable out-of-class after #4868.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_ref_arg_bare_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base(x: int) "
        "  public readonly property X: int := x; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x); "
        "    return d.X; "
        "  end Main; "
        "end C;"), 10);
}

// §22.3 / fix #2584: MIXED ref + value params — `base(n + k)` where n is ref
// (bound via refParams_) and k is a plain value param (bound via paramSlots_).
// Confirms the fix's per-param branch: ref params go to refParams_, plain
// params keep paramSlots_. Pre-fix: malformed IR (n loaded as ptr).
// Post-fix: d.X = 10 + 5 = 15. Per §22.10 (v0.307) post-#4868: Base.x is not
// auto-promoted, so the test reads d.X via an explicit property declaration.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_ref_arg_mixed_ref_and_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base(x: int) "
        "  public readonly property X: int := x; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int, k: int) base(n + k); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x, 5); "
        "    return d.X; "
        "  end Main; "
        "end C;"), 15);
}

// §22.3 / fix #2584: the chain arg reads the ref param (n=10 -> Base.x=11),
// then the derived ctor BODY mutates the alias (n := n + 100 -> caller's
// x=110). Confirms the chain read the RIGHT initial value (the fix) AND the
// alias stays live into the body (bindParameters' refParams_). Pre-fix:
// malformed IR. Post-fix: caller's x = 110. Base.x is declared as a public
// readonly property (per §22.10 idiom, v0.307) so the test could read it
// directly — but this row observes the caller's x after the alias mutation,
// not Base.X, so the property declaration is bookkeeping only here.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_ref_arg_chain_then_body_mutate) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base(x: int) "
        "  public readonly property X: int := x; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(n + 1); n := n + 100; end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x); "
        "    return x; "
        "  end Main; "
        "end C;"), 110);
}

// =============================================================================
// Control: a VALUE param chain-call arg to a primary-ctor base — must keep
// working (a false reject of a working chain is worse than the bug). The fix
// gates the refParams_ binding on `isByRefMod(ps->modifier)`, so a plain param
// keeps the paramSlots_ path. Pre- and post-fix: 11.
// =============================================================================

// §12.8 / fix #2584: `base(m + 1)` with a plain value param m — no ref/out, no
// refParams_ binding; the existing paramSlots_ value-passing path. Pinned so
// the fix does not regress it. Per §22.10 (v0.307) post-#4868 the test reads
// the base's primary-ctor param via an explicit `public readonly property X`.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_value_param_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base(x: int) "
        "  public readonly property X: int := x; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(m: int) base(m + 1); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived(10); "
        "    return d.X; "
        "  end Main; "
        "end C;"), 11);
}

// =============================================================================
// Control: #2579's NON-primary-ctor `base(ref n)` case — the arg IS the ref
// param (passed by address through emitChainCall), against a base with an
// EXPLICIT ctor body. A different site (emitChainCall, not
// prePopulateBasePositionalFields); pinned to confirm the two fixes do not
// collide. Pre- and post-fix: 15.
// =============================================================================

// §22.3 / fix #2579 (control for #2584): `base(ref n)` to a NON-primary-ctor
// base — emitChainCall passes the address; the Base ctor body's `n := n + 5`
// mutates the caller's x. Must stay working after the #2584 fix.
BEZEL_TEST(ref_out_primary_base_chain_arg_2584, base_ref_non_primary_base_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public _v: int; "
        "  public constructor(ref n: int) n := n + 5; _v := n; end constructor; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(ref n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x); "
        "    return x; "
        "  end Main; "
        "end C;"), 15);
}
