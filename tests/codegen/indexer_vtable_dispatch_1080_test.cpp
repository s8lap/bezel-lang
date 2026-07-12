// Regression tests for #1080 (§23.4 / §12.1): a virtual/override/abstract
// user-defined INDEXER getter (and setter) dispatched STATICALLY for class
// receivers and `this[i]`, so a `base[i]` read through a base-typed reference,
// and a `this[i]` read inside a base-class method, ran the wrong (base /
// abstract) implementation. A direct `obj[i]` on the concrete type already
// dispatched correctly.
//
// Root cause: indexers were excluded from vtable slots (isVirtualProperty
// returns false for indexers), so there was no slot to dispatch through and
// lowerIndexerGet resolved the getter body from the compile-time receiver type.
// The fix mirrors the #481 property-vtable path: virtual indexers get vtable
// slots (keyed by index signature so overloads stay distinct), filled with the
// concrete class's indexer getter/setter, and lowerIndexerGet/Set dispatch
// through the slot for class receivers.
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

// #1080 form A: `base[i]` through a base-typed reference dispatches to the
// most-derived override.
BEZEL_TEST(indexer_vtable_dispatch_1080, base_ref_get_dispatches_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property [i: int]: int => 1; end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int => 99; end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  return b[0]; "                                // 99 (was 1)
        "end Main; end C;"), 99);
}

// #1080 form B: `this[i]` inside an (abstract) base-class method dispatches to
// the concrete override.
BEZEL_TEST(indexer_vtable_dispatch_1080, this_index_in_base_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class ABase "
        "  public abstract property [i: int]: int; "
        "  public method GetZero(): int => this[0]; "
        "end ABase; "
        "class Concrete: ABase "
        "  public override property [i: int]: int => 42; end Concrete; "
        "class C public static method Main(): int "
        "  using var c := new Concrete(); "
        "  return c.GetZero(); "                         // 42 (was 0)
        "end Main; end C;"), 42);
}

// The SETTER also dispatches dynamically: a `base[i] := v` through a base-ref
// runs the override's setter, and the subsequent read runs the override getter.
BEZEL_TEST(indexer_vtable_dispatch_1080, base_ref_setter_dispatches_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _v: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _v + 1; "
        "    set => do _v := value; end; "
        "  end; "
        "end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int "
        "    get => _v + 100; "
        "    set => do _v := value * 2; end; "
        "  end; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  b[0] := 5; "                                  // override setter → _v = 10
        "  return b[0]; "                                // override getter → 110
        "end Main; end C;"), 110);
}

// Regression guard: a direct read on the concrete type is unchanged.
BEZEL_TEST(indexer_vtable_dispatch_1080, direct_override_read_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property [i: int]: int => 1; end Base; "
        "class Derived: Base "
        "  public override property [i: int]: int => 42; end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d[0]; "                                // 42
        "end Main; end C;"), 42);
}

// Regression guard: a plain (non-virtual) indexer is unchanged (static path).
BEZEL_TEST(indexer_vtable_dispatch_1080, non_virtual_indexer_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 7; "
        "  public property [i: int]: int => _v + i; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  return b[3]; "                                // 10
        "end Main; end C;"), 10);
}

// Overloaded virtual indexers (`[i: int]` / `[s: bool]`) get DISTINCT vtable
// slots (keyed by index signature) so each dispatches to its own getter.
BEZEL_TEST(indexer_vtable_dispatch_1080, overloaded_virtual_indexers_distinct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual property [i: int]: int => 1; "
        "  public virtual property [s: bool]: int => 2; "
        "end Base; "
        "class C public static method Main(): int "
        "  using var b := new Base(); "
        "  return b[0] + b[true]; "                      // 1 + 2 = 3
        "end Main; end C;"), 3);
}
