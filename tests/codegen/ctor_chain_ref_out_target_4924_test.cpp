// Regression tests for issue #4924 — codegen's constructor selection
// (`ctorForArgs`) ignored the call-site `ref`/`out` modifier, so a
// `this(...)` / `base(...)` chain could call an overload that §22.3 makes
// INAPPLICABLE to the call. (The `new` sites share the selector and the gap;
// they are deliberately out of this fix's scope — see the ctorForArgs
// comment.)
//
// spec §22.3 "Parameter modifiers" (the caller writes `ref`/`out` at the call
// site, and only for a `ref`/`out` parameter), §22.6 (only applicable
// candidates are ranked), §22.5 / §12.8 (the chain call's arguments bind the
// target's formals), v0.307.
//
// Pre-fix, `ctorForArgs` took the first candidate whose argument TYPE CLASSES
// matched: `this(n, 5)` beside `(ref y: int, x: int)` and `(y: long, x: int)`
// called the `ref` overload (printed `R5,3`, correct `V5,3`); `this(ref n, 5)`
// with the value overload declared first called the value overload; a
// non-lvalue argument (`this(s.Length, 6)`) reached the `ref` overload and
// ICEd ("internal: lowerExpr returned null for base-constructor argument —
// #1386"). Every row except the control fails pre-fix; the expected text is
// what the §22.3-applicable candidate prints.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §22.3 / §22.6 (v0.307), fix #4924 — the issue's repro: a plain
// `this(n, 5)` must skip the `ref y` overload and call `(y: long, x: int)`.
BEZEL_TEST(ctor_chain_ref_out_target_4924, this_plain_call_skips_ref_overload_issue_repro) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int, x: int) Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(y: long, x: int) Tag := $\"V{x},{y}\"; end constructor; "
        "  public constructor(n: int) this(n, 5); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(3); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "V5,3\n");
}

// per §22.3 / §22.6 (v0.307), fix #4924 — the other direction: `this(ref n, 5)`
// must call the `ref` overload even when the value overload is declared first.
BEZEL_TEST(ctor_chain_ref_out_target_4924, this_ref_call_skips_value_overload_declared_first) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(y: long, x: int) Tag := $\"V{x},{y}\"; end constructor; "
        "  public constructor(ref y: int, x: int) y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(n: int) this(ref n, 5); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(3); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "R5,4\n");
}

// per §22.3 / §22.6 (v0.307), fix #4924 — base(...): `base(n)` must call
// `(y: int, s: string := "d")`, not `(ref y: int)`; `base(ref n)` the latter.
// (The L22 reviewer's s5 probe.)
BEZEL_TEST(ctor_chain_ref_out_target_4924, base_plain_call_skips_ref_overload) {
    expectPrints(
        "module M; "
        "class B "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int) y := y + 1; Tag := $\"R{y}\"; end constructor; "
        "  public constructor(y: int, s: string := \"d\") Tag := $\"P{y}{s}\"; end constructor; "
        "end B; "
        "class D : B "
        "  public constructor(n: int) base(ref n); end constructor; "
        "end D; "
        "class E : B "
        "  public constructor(n: int) base(n); end constructor; "
        "end E; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); Console.WriteLine(d.Tag); "
        "    using var e := new E(4); Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "R5\nP4d\n");
}

// per §22.3 / §22.6 (v0.307), fix #4924 — base(...), other direction:
// `base(ref n)` must call `(ref y: int)` though `(y: long)` is declared
// first; `base(n)` calls `(y: long)`.
BEZEL_TEST(ctor_chain_ref_out_target_4924, base_ref_call_skips_value_overload_declared_first) {
    expectPrints(
        "module M; "
        "class B "
        "  public readonly property Tag: string; "
        "  public constructor(y: long) Tag := $\"V{y}\"; end constructor; "
        "  public constructor(ref y: int) y := y + 1; Tag := $\"R{y}\"; end constructor; "
        "end B; "
        "class D : B "
        "  public constructor(n: int) base(ref n); end constructor; "
        "end D; "
        "class E : B "
        "  public constructor(n: int) base(n); end constructor; "
        "end E; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); Console.WriteLine(d.Tag); "
        "    using var e := new E(4); Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "R5\nV4\n");
}

// per §22.3 / §22.6 (v0.307), fix #4924 — a non-lvalue plain argument
// (`this(s.Length, 6)`) reached the `ref` overload and ICEd with #1386
// pre-fix; it must call `(y: int, x: int)`. (The L22 reviewer's s1 probe.)
BEZEL_TEST(ctor_chain_ref_out_target_4924, this_plain_non_lvalue_arg_no_ice) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int, x: int) "
        "    y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(y: int, x: int) "
        "    Tag := $\"P{x},{y}\"; end constructor; "
        "  public constructor(n: int) this(ref n, 5); end constructor; "
        "  public constructor(s: string) this(s.Length, 6); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var a := new J(3); Console.WriteLine(a.Tag); "
        "    using var b := new J(\"ab\"); Console.WriteLine(b.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "R5,4\nP6,2\n");
}

// per §22.3 / §22.6 / §22.10 (v0.307), fix #4924 — a plain `base(n, 5)` must
// bind the base's primary list `(p, q)`, not its `(ref y: int, x: long)` body
// constructor; `base(ref n, 5)` calls the latter. (The `ref` constructor's
// type list is deliberately NOT the primary's `(int, int)`: whether a `ref`
// constructor may share its type list with the primary is the open §9.3
// question #4775 Q1, and this row must not depend on its answer.)
BEZEL_TEST(ctor_chain_ref_out_target_4924, base_plain_call_binds_primary_not_ref_ctor) {
    expectPrints(
        "module M; "
        "class B(p: int, q: int) "
        "  public constructor(ref y: int, x: long) this(y, 7); y := 100; end constructor; "
        "  public method Show() Console.WriteLine($\"p={p} q={q}\"); end Show; "
        "end B; "
        "class D : B "
        "  public constructor(n: int) base(n, 5); end constructor; "
        "end D; "
        "class E : B "
        "  public readonly property N: int; "
        "  public constructor(n: int) base(ref n, 5); N := n; end constructor; "
        "end E; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); d.Show(); "
        "    using var e := new E(4); e.Show(); Console.WriteLine(e.N.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "p=4 q=5\np=4 q=7\n100\n");
}

// per §22.3 / §22.5 / §22.6 / §22.10 (v0.307), fix #4924 (rework) — a
// `base(ref n)` must not bind the primary `(p: int)`: a primary parameter is
// never `ref`, so the primary is inapplicable and the call reaches the
// defaulted-tail `(ref y: int, k: int := 7)` constructor. Pre-rework the
// primary's own applicability check ignored the modifier, so `ref n` was bound
// to slot `p` and the program printed `p=4` / `4` (reviewer probe p1).
BEZEL_TEST(ctor_chain_ref_out_target_4924, base_ref_call_skips_primary_for_defaulted_ref_ctor) {
    expectPrints(
        "module M; "
        "class B(p: int) "
        "  public constructor(ref y: int, k: int := 7) this(y + k); y := 100; end constructor; "
        "  public method Show() Console.WriteLine($\"p={p}\"); end Show; "
        "end B; "
        "class D : B "
        "  public readonly property N: int; "
        "  public constructor(n: int) base(ref n); N := n; end constructor; "
        "end D; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); d.Show(); Console.WriteLine(d.N.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "p=11\n100\n");
}

// per §22.3 / §22.5 / §22.6 / §22.10 (v0.307), fix #4924 (rework) — the `out`
// twin of the row above: `base(out n)` must reach the defaulted-tail
// `(out y: int, k: int := 7)` constructor, not the primary `(p: int)`.
BEZEL_TEST(ctor_chain_ref_out_target_4924, base_out_call_skips_primary_for_defaulted_out_ctor) {
    expectPrints(
        "module M; "
        "class B(p: int) "
        "  public constructor(out y: int, k: int := 7) this(k); y := 100; end constructor; "
        "  public method Show() Console.WriteLine($\"p={p}\"); end Show; "
        "end B; "
        "class D : B "
        "  public readonly property N: int; "
        "  public constructor(n: int) base(out n); N := n; end constructor; "
        "end D; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); d.Show(); Console.WriteLine(d.N.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "p=7\n100\n");
}

// per §22.3 (v0.307), fix #4924 — CONTROL (passes pre- and post-fix): the
// `ref` overload, declared first, is correctly chosen for `this(ref n, 5)`
// and the ref write-back is visible.
BEZEL_TEST(ctor_chain_ref_out_target_4924, control_this_ref_call_picks_ref_overload_declared_first) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int, x: int) y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(y: long, x: int) Tag := $\"V{x},{y}\"; end constructor; "
        "  public constructor(n: int) this(ref n, 5); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(3); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "R5,4\n");
}
