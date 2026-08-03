// Spec-derived tests for issue #2579 — a `base(ref x)` / `this(ref x)`
// CONSTRUCTOR-CHAIN call passed the argument's LOADED VALUE instead of its
// ADDRESS, emitting malformed IR that LLVM's `verifyModule` rejects
// ("Call parameter type does not match function signature! … call void
// @"bz$M.Base$ctor"(ptr %0, i32 %2)" — the compiled chain-target ctor's
// signature uses `ptr` for `ref`/`out` params (CG50), but the chain-call arg
// loop in `LlvmBackend_Setup.cpp` `Emitter::emitChainCall` lowered each arg
// with a bare `lowerExpr`).
//
// §22.3 (v0.236), `ref` and `out` argument locations:
//   "A `ref` argument naming a mutable local or writable field is a true
//    alias: callee writes are visible through the caller's location
//    immediately … An indexer slot or property target is instead copy-in /
//    copy-out under §17.6's evaluate-once discipline … on normal return the
//    setter is called once with the slot's final value."
// §22.3 puts NO restriction on `ref`/`out` in a constructor-chain call, and
// sema admits `base(ref n)` / `this(ref n)` (Resolver::checkConstructorChains,
// PureBodyWalker::checkPureCtorChainPurity), so a §22.3-conformant program was
// wrongly rejected. The fix wires the §22.3 bracket (and the alias-path
// `lvalueAddr`) at the chain-call site, mirroring what #1302/#2319 did for the
// method-call and constructor-call paths.
//
// Pre-fix behavior (mechanically confirmed on origin/main, commit 738035a):
//   * `base(ref n)` → malformed-IR compile error (`.ran=false`); the program
//     never ran, so no wrong value was produced — `verifyModule` aborted the
//     build. Every alias/bracket chain-call case below therefore fails with
//     `run() == -999` on the pre-fix compiler.
//   * The generic-base inline path additionally loaded the ref param's pointer
//     inside the inlined body (#2283's shape on the chain-call path); the fix
//     registers the address in `refParams_` so body reads/writes dereference.
//
// `ref_out_ctor_accessor_2319_test.cpp` covers the NON-chain constructor-call
// path (`new Ctor(ref arr[0])`); these tests cover the CHAIN path
// (`base(ref …)` / `this(ref …)`), which has its own arg loop and a separate
// inline path for a generic base.

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
// The cardinal fix (issue #2579 repro): `base(ref n)` alias path on a
// non-generic base — the direct compiled-ctor call path. Pre-fix: malformed
// IR (the loaded i32 passed to a ptr param). Post-fix: rc=15.
// =============================================================================

// §22.3 / fix #2579: the issue's exact repro — `base(ref n)` aliases the
// caller's local through the chain call, so the Base ctor's `n := n + 5`
// mutates `x`. Pre-fix: malformed IR, run()==-999. Post-fix: 10 + 5 = 15.
BEZEL_TEST(ref_out_chain_call_2579, base_ref_local_aliases) {
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

// §22.3 / fix #2579: the `out` sibling — `base(out n)` writes through the
// alias; "for `out` no getter call occurs" (irrelevant for a true alias) and
// the callee must assign on every normal path (§22.3). Pre-fix: malformed IR.
// Post-fix: 42.
BEZEL_TEST(ref_out_chain_call_2579, base_out_local_aliases) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public _v: int; "
        "  public constructor(out n: int) n := 42; _v := n; end constructor; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(out n: int) base(out n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x: int := 0; "
        "    using var d := new Derived(out x); "
        "    return x; "
        "  end Main; "
        "end C;"), 42);
}

// =============================================================================
// `this(ref n)` delegation chain (same emitChainCall site, isThis=true).
// =============================================================================

// §22.3 / fix #2579: `this(ref n)` delegates to a sibling ctor on the SAME
// class; the ref param aliases through the chain. Pre-fix: malformed IR.
// Post-fix: 10 + 5 = 15.
BEZEL_TEST(ref_out_chain_call_2579, this_ref_local_aliases) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class C "
        "  public _v: int; "
        "  public constructor(ref n: int) n := n + 5; _v := n; end constructor; "
        "  public constructor(ref n: int, dummy: int) this(ref n); end constructor; "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var c := new C(ref x, 0); "
        "    return x; "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Generic base — the chain-call INLINE path (emitChainCall inlines the
// target ctor body under the active generic substitution). Pre-fix the inline
// path bound the ref param via paramSlots_, so the body loaded the pointer
// itself (the #2283 shape); the fix registers the address in refParams_.
// =============================================================================

// §22.3 / fix #2579: `base(ref n)` to a GENERIC base — the inline path. The
// Base ctor's `n := n + 5` must mutate the caller's `x` through the alias.
// Pre-fix: malformed IR. Post-fix: 10 + 5 = 15.
BEZEL_TEST(ref_out_chain_call_2579, base_ref_generic_base_inline) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class GBase<T> "
        "  public _v: int; "
        "  public constructor(ref n: int) n := n + 5; _v := n; end constructor; "
        "end GBase; "
        "public class Derived: GBase<int> "
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

// =============================================================================
// The §22.3 bracket (copy-in / copy-out) on a chain call — an accessor-
// mediated location (a property with a setter). Mirrors the #1302/#2319
// bracket on the method/ctor-call paths.
// =============================================================================

// §22.3 / fix #2579: `base(ref h.V)` where V is a block-form property — the
// getter runs once (copy-IN, reads 10), the Base ctor mutates the slot
// (10+5=15), and on normal return the setter writes 15 back to `_back`.
// Pre-fix: malformed IR. Post-fix: 15.
BEZEL_TEST(ref_out_chain_call_2579, base_ref_property_bracket_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Holder "
        "  public _back: int; "
        "  public property V: int "
        "    get => _back; "
        "    set => _back := value; "
        "  end; "
        "  public constructor() _back := 10; end constructor; "
        "end Holder; "
        "public class Base "
        "  public _v: int; "
        "  public constructor(ref n: int) n := n + 5; _v := n; end constructor; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(ref n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var d := new Derived(ref h.V); "
        "    return h._back; "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Bidirectional alias: the base ctor READS n before writing it — confirms the
// alias carries the caller's value IN as well as the mutation OUT.
// =============================================================================

// §22.3 / fix #2579: Base reads n (10) into _v, then n := n + 100 (110). The
// caller observes x=110 and d._v=10, returning 120. Pre-fix: malformed IR.
BEZEL_TEST(ref_out_chain_call_2579, base_ref_reads_then_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public _v: int; "
        "  public constructor(ref n: int) _v := n; n := n + 100; end constructor; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(ref n: int) base(ref n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var d := new Derived(ref x); "
        "    return x + d._v; "
        "  end Main; "
        "end C;"), 120);
}

// =============================================================================
// Control: a VALUE param chain call — must keep working (a false reject of a
// working chain is worse than the bug). Pre- and post-fix: 11.
// =============================================================================

// §12.8: `base(n)` with a plain value param — no §22.3 bracket, no address;
// the existing value-passing path. Pinned so the fix does not regress it.
BEZEL_TEST(ref_out_chain_call_2579, base_value_param_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public _v: int; "
        "  public constructor(n: int) _v := n + 1; end constructor; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor(n: int) base(n); end constructor; "
        "end Derived; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new Derived(10); "
        "    return d._v; "
        "  end Main; "
        "end C;"), 11);
}
