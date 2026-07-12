// Regression tests for #1090 (§12.1 / §22.5 / §22.6): overriding a SUBSET of
// inherited overloaded virtual methods, and resolving a call to an inherited
// overload, were wrongly rejected. Three sema sites keyed the overload set by
// bare name within a single class:
//   1. checkHidingAndOverride matched a derived override against whichever base
//      overload was first in declaration order (a per-name map, first-wins), so
//      `override F(x, y)` when `F(x)` is declared first reported a false
//      param-count mismatch (§12.1).
//   2. resolveMethodOverload collected candidates only from the one class
//      lookupMember stopped at, so an overload declared only in a base class was
//      invisible and a valid call to it was mis-rejected.
//   3. checkCallArity counted same-name methods in one class only, so a call to
//      an inherited overload was arity-checked against the sole-overload
//      assumption — "supplies 2 args but the parameter list has 1" (§22.5).
// The codegen vtable-slot-collision half (same-class overloaded dispatch) was
// fixed earlier in #1085 (c69377c); this is the sema completion.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1090 site 2/3: a class overriding ONE inherited overload can still call the
// OTHER (inherited) overload, and the override dispatches correctly.
//   d.F(3, 4) → Base.F(x, y) = 7;  d.F(5) → Derived's override F(x) = 15 → 22.
BEZEL_TEST(virtual_overload_inherited_1090, subset_override_calls_inherited) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => x * 2; "
        "  public virtual method F(x: int, y: int): int => x + y; "
        "end Base; "
        "class Derived: Base "
        "  public override method F(x: int): int => x * 3; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.F(3, 4) + d.F(5); "                  // 7 + 15 = 22
        "end Main; end C;"), 22);
}

// #1090 site 1: overriding the NON-first-declared overload (`override F(x, y)`
// when `F(x)` is declared first) is matched to the right base overload by
// signature — no false §12.1 param-count mismatch.  d.F(10, 20) = 31.
BEZEL_TEST(virtual_overload_inherited_1090, override_non_first_declared) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => x * 2; "
        "  public virtual method F(x: int, y: int): int => x + y; "
        "end Base; "
        "class Derived: Base "
        "  public override method F(x: int, y: int): int => x + y + 1; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.F(10, 20); "                          // 10 + 20 + 1 = 31
        "end Main; end C;"), 31);
}

// The overridden overload dispatches virtually through a base-typed reference,
// and the inherited (un-overridden) overload is callable through it too.
//   b.F(10) → Derived's override F(x) = 30;  b.F(1, 2) → Base.F(x, y) = 3 → 33.
BEZEL_TEST(virtual_overload_inherited_1090, virtual_dispatch_via_base_ref) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => x * 2; "
        "  public virtual method F(x: int, y: int): int => x + y; "
        "end Base; "
        "class Derived: Base "
        "  public override method F(x: int): int => x * 3; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  return b.F(10) + b.F(1, 2); "                  // 30 + 3 = 33
        "end Main; end C;"), 33);
}

// A 3-level hierarchy: each level overrides a DIFFERENT overload; the
// most-derived override of each is selected, and an overload inherited two
// levels up is still visible.
//   gc.F(7) → Derived's override F(x) = 70;  gc.F(2, 3) → GrandChild's
//   override F(x, y) = 6 → 76.
BEZEL_TEST(virtual_overload_inherited_1090, three_level_hierarchy) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => x; "
        "  public virtual method F(x: int, y: int): int => x + y; "
        "end Base; "
        "class Derived: Base "
        "  public override method F(x: int): int => x * 10; "
        "end Derived; "
        "class GrandChild: Derived "
        "  public override method F(x: int, y: int): int => x * y; "
        "end GrandChild; "
        "class C public static method Main(): int "
        "  using var gc := new GrandChild(); "
        "  return gc.F(7) + gc.F(2, 3); "                 // 70 + 6 = 76
        "end Main; end C;"), 76);
}

// A call matching NO overload across the hierarchy is still rejected (the
// arity check must not be silently skipped just because the name is overloaded).
BEZEL_TEST(virtual_overload_inherited_1090, no_matching_overload_rejected) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => x; "
        "  public virtual method F(x: int, y: int): int => x + y; "
        "end Base; "
        "class Derived: Base "
        "  public override method F(x: int): int => x; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.F(1, 2, 3); "                         // no 3-arg overload
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Regression guard: same-class type-based overloads (one class, F(int) vs
// F(string)) still resolve by §22.6 exactly as before.
//   c.G(5) = 6;  c.G("hi") = 100 → 106.
BEZEL_TEST(virtual_overload_inherited_1090, same_class_type_overloads_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public method G(x: int): int => x + 1; "
        "  public method G(s: string): int => 100; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.G(5) + c.G(\"hi\"); "                // 6 + 100 = 106
        "  end Main; "
        "end C;"), 106);
}
