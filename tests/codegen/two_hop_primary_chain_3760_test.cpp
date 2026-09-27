// Regression tests for #3760 (§22.10 / §12.8 / §22.8, v0.287): a two-hop
// `this(...)` chain on a primary-constructor class — convenience ctor A chains
// `this(...)` to convenience ctor B, which chains `this(...)` to the primary
// — generated IR that looped back and overflowed the stack at run time. The
// bug was that `Emitter::ctorForArgs` (LlvmBackend_impl.hpp) returned the
// single arity-matched explicit sibling WITHOUT a type-class check on its
// parameter list, so a `this(int)` from B (the (a, b) ctor) in
// `class C(x: int)` was routed to the (s: string) sibling by arity alone —
// (s) chained back to (a, b), forming a closed (a, b) ↔ (s) loop.
//
// The fix factors the type-class check that the >1-candidate arm already ran
// into a lambda the ==1-candidate arm also consults, so a SINGLE arity-match
// whose param types disagree with the call's args is REJECTED and the call
// falls through to the §22.10 primary-ctor arm (LlvmBackend_Setup.cpp:12189,
// fix #1087) — which stores the chained positional args into the param-
// fields and runs the base/field-init sequence, exactly the path a single-hop
// `this(int)` from a (a, b) ctor took before.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// runAot returns a JitResult; row 1 needs r.ran + r.stdoutText (the int is
// observable through Console.WriteLine, not as a Main exit code, because the
// test program's primary purpose is to print the chained value rather than
// return it).
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

std::string runOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — the issue's repro (#3760): convenience A chains this(int, int)
// to convenience B which chains this(int) to the primary. Before the fix,
// this raised System.OverflowException at run time (the chain looped
// between A and B); after the fix, the int correctly reads through the
// chain and prints. Measured at r12-cycle-1 (5d1b212): pre-fix `4` did not
// print and the test program trapped at SIGABRT inside `calloc` after
// stack exhaustion. Post-fix: prints "4" and exits 0.
// ---------------------------------------------------------------------------
// §22.10 (v0.307, line 1861): post-#4868, a class primary-ctor parameter is
// not auto-promoted to a public property. The test reads `c.X` via an explicit
// `public readonly property X` declaration (spec §11 / §22.10 idiom,
// docs/Bezel_Language_Spec.md:2042-2057).
BEZEL_TEST(two_hop_primary_chain_3760, two_hop_chain_to_primary_runs) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "class C(x: int) "
        "  public readonly property X: int := x; "
        "  public constructor(a: int, b: int) this(a + b); end constructor; "
        "  public constructor(s: string) this(s.Length, 1); end constructor; "
        "end C; "
        "class MainClass "
        "  public static method Main(): int "
        "    var c := new C(\"abc\"); "
        "    Console.WriteLine(c.X.ToString()); "
        "    return 0; "
        "  end Main; "
        "end MainClass;"), std::string("4\n"));
}

// ---------------------------------------------------------------------------
// Row 2 — control: a single-hop `this(int)` chain on a primary-ctor class
// (no sibling at matching arity). Pre-fix and post-fix: works (the §22.10
// primary arm was the path). This row pins that the fix didn't disturb the
// single-hop case.
//
// Per spec §22.10 (v0.307, line 1861) post-#4868: the class's primary-ctor
// parameter `x` is not auto-promoted to a public property. Migration adds the
// explicit `public readonly property X` (canonical idiom, lines 2042-2057) so
// the chained value can be observed out-of-class as `c.X`.
// ---------------------------------------------------------------------------
BEZEL_TEST(two_hop_primary_chain_3760, single_hop_chain_still_works) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "class C(x: int) "
        "  public readonly property X: int := x; "
        "  public constructor(a: int) this(a + 1); end constructor; "
        "end C; "
        "class MainClass "
        "  public static method Main(): int "
        "    var c := new C(a := 10); "
        "    Console.WriteLine(c.X.ToString()); "
        "    return 0; "
        "  end Main; "
        "end MainClass;"), std::string("11\n"));
}

// ---------------------------------------------------------------------------
// Row 3 — control: an explicit `base(...)` chain from a derived class with
// a primary base constructor. No `this(...)` involvement at all; pins the
// fix didn't perturb the base-chain arm. Per §22.10 (v0.307) post-#4868 the
// base's primary-ctor parameter `x` is not auto-promoted, but this row reads
// the value through `d.GetX()` (a method body access, still permitted) — no
// property declaration needed.
// ---------------------------------------------------------------------------
BEZEL_TEST(two_hop_primary_chain_3760, explicit_base_chain_still_works) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "class P(x: int) "
        "  public method GetX(): int => x; "
        "end P; "
        "class D : P "
        "  public constructor(v: int) base(v); end constructor; "
        "end D; "
        "class MainClass "
        "  public static method Main(): int "
        "    var d := new D(99); "
        "    Console.WriteLine(d.GetX().ToString()); "
        "    return 0; "
        "  end Main; "
        "end MainClass;"), std::string("99\n"));
}

// ---------------------------------------------------------------------------
// Row 4 — control: a THREE-hop `this(...)` chain on a primary-ctor class
// where each hop resolves to a sibling by arity AND type. `new C()` →
// parameterless → (a: int) → (a, b) → primary (x, y, z). Each hop's args
// type-match the next target's params (no `(s: string)` sibling pulling the
// chain off-course), so the >1-candidate path's type-class check was the
// load-bearing one for this row. Post-fix: prints "5/6/7" — the three
// stored positional args.
//
// Per §22.10 (v0.307, line 1861) post-#4868, the three primary-ctor parameters
// `x`/`y`/`z` are not auto-promoted. Migration declares three explicit
// `public readonly property` accessors (canonical idiom, lines 2042-2057) so
// the chained values are observable out-of-class as `c.X`/`c.Y`/`c.Z`.
// ---------------------------------------------------------------------------
BEZEL_TEST(two_hop_primary_chain_3760, three_hop_chain_to_primary_runs) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "class C(x: int, y: int, z: int) "
        "  public readonly property X: int := x; "
        "  public readonly property Y: int := y; "
        "  public readonly property Z: int := z; "
        "  public constructor(a: int) this(a, a + 1, a + 2); end constructor; "
        "  public constructor(a: int, b: int) this(a, b, 0); end constructor; "
        "  public constructor() this(5); end constructor; "
        "end C; "
        "class MainClass "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine($\"{c.X}/{c.Y}/{c.Z}\"); "
        "    return 0; "
        "  end Main; "
        "end MainClass;"), std::string("5/6/7\n"));
}

// ---------------------------------------------------------------------------
// Row 5 — control: the circular self-chain shape from #3793
// (`tests/sema/ctor_self_chain_3793_test.cpp`) is still rejected. `this(a)`
// in a single `constructor(a: int)` of a class with NO primary ctor and NO
// other ctors must produce the §22.10 "a constructor cannot chain to its
// own constructor" diagnostic — and the program MUST NOT run. The pre-#3793
// path silently emitted nothing and printed 0; the #3760 fix did not touch
// sema's check, but this row pins that the new arity+type resolver still
// leaves sema's reject in place.
// ---------------------------------------------------------------------------
BEZEL_TEST(two_hop_primary_chain_3760, self_chain_still_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  public constructor(a: int) "
        "    this(a); "
        "  end constructor; "
        "  public method GetA(): int => this._a; "
        "end B; "
        "class MainClass "
        "  public static method Main(): int "
        "    var b := new B(7); "
        "    Console.WriteLine(b.GetA().ToString()); "
        "    return 0; "
        "  end Main; "
        "end MainClass;");
    // Sema must reject with the §22.10 self-chain diagnostic. The exact
    // diagnostic text is shared with ctor_self_chain_3793_test.cpp; here
    // we look for the §22.10 citation alone — the brief said "control,
    // must reject at sema per #3793", and any §22.10 error satisfies it.
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§22.10") != std::string::npos) { found = true; break; }
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_FALSE(r.ran);
}