// Regression tests for #1085 (§12.1): overloaded VIRTUAL methods sharing a name
// but differing by arity were assigned the SAME vtable slot (buildVtable keyed
// the slot by the bare method name), so the second overload overwrote the
// first's slot (last-writer-wins) and every call to an earlier overload
// dispatched to the wrong body. The fix keys the vtable slot by name+arity, so
// each overload gets its own slot; dispatch (keyed by MethodSymbol*) then finds
// the right function. Single-overload virtual/override dispatch is unaffected
// (distinct names → distinct keys either way; only the override-reuse lookup
// changed, and a base/derived override of the same arity keeps the same key).
//
// runJit returns the raw exit code (0..255, 8-bit wrap).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kTwo =
    "module M; class C "
    "  public virtual method F(x: int): int => 11; "
    "  public virtual method F(x: int, y: int): int => 22; ";
}  // namespace

// The #1085 repro: the 1-arg virtual overload dispatches to its own body, not
// the last-declared 2-arg overload.
BEZEL_TEST(virtual_overload_dispatch_1085, one_arg_overload_dispatches_correctly) {
    BEZEL_EXPECT_EQ(run(std::string(kTwo) +
        "  public static method Main(): int "
        "    using var c := new C(); return c.F(5); "   // 11, not 22
        "  end Main; end C;"), 11);
}

// The 2-arg overload still reaches its own body too.
BEZEL_TEST(virtual_overload_dispatch_1085, two_arg_overload_dispatches_correctly) {
    BEZEL_EXPECT_EQ(run(std::string(kTwo) +
        "  public static method Main(): int "
        "    using var c := new C(); return c.F(5, 6); "  // 22
        "  end Main; end C;"), 22);
}

// Three virtual overloads — each of the three arities dispatches to its own
// body. Bodies 1/2/4 (distinct bits) so any mis-dispatch changes the sum;
// 1 + 2 + 4 = 7.
BEZEL_TEST(virtual_overload_dispatch_1085, three_overloads_each_distinct) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public virtual method F(x: int): int => 1; "
        "  public virtual method F(x: int, y: int): int => 2; "
        "  public virtual method F(x: int, y: int, z: int): int => 4; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.F(5) + c.F(5, 6) + c.F(5, 6, 7); "
        "  end Main; end C;"), 7);
}

// Calling a virtual overload through a BASE reference dispatches to the derived
// override of the SAME arity (the override reuses the base slot via the
// name+arity key); the other-arity overload is unaffected.
BEZEL_TEST(virtual_overload_dispatch_1085, overload_through_base_ref) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method F(x: int): int => 1; "
        "  public virtual method F(x: int, y: int): int => 2; "
        "end Base; "
        "class C public static method Main(): int "
        "  using var b := new Base(); "
        "  return b.F(7) * 10 + b.F(7, 8); "        // 1*10 + 2 = 12
        "end Main; end C;"), 12);
}

// A single-overload virtual override through a base reference is unchanged
// (regression guard for the vtable-layout invariant).
BEZEL_TEST(virtual_overload_dispatch_1085, single_overload_override_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method G(): int => 1; end Base; "
        "class Der: Base public override method G(): int => 9; end Der; "
        "class C public static method Main(): int "
        "  using var d := new Der(); var b: Base := d; return b.G(); "  // 9
        "end Main; end C;"), 9);
}
