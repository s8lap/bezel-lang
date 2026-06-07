// Codegen tests for bug #478 — a property/field initializer that references a
// primary-constructor parameter must compile AND produce the correct runtime
// value. Authority: §22.8 / §22.10.
//
// The repro from bug #478:
//   class Point(x: int, y: int)
//     public readonly property X: int := x;
//     public readonly property Y: int := y;
//   end Point;
//   new Point(3, 4) → p.X + p.Y == 7.
//
// per §22.10: primary-ctor params are in scope throughout the class body
// including field initializers. Values flow directly into initialized fields
// (no capture when only referenced in initializers per §22.10 capture rule).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §22.8 / §22.10 — the exact repro from bug #478: `new Point(3,4)` → X+Y=7.
BEZEL_TEST(codegen_primary_ctor_param_init_478, point_xy_sum_spec_22_10) {
    // per §22.8 / §22.10
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Point(x: int, y: int) "
        "  public readonly property X: int := x; "
        "  public readonly property Y: int := y; "
        "end Point; "
        "class C public static method Main(): int "
        "  using var p := new Point(3, 4); "
        "  return p.X + p.Y; "
        "end Main; end C;"), 7);
}

// §22.10 — field (not property) initialized from a primary-ctor param.
BEZEL_TEST(codegen_primary_ctor_param_init_478, field_init_from_primary_ctor_param_22_10) {
    // per §22.8 / §22.10
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box(value: int) "
        "  public _stored: int := value; "
        "  public method Get(): int => _stored; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(42); "
        "  return b.Get(); "
        "end Main; end C;"), 42);
}

// §22.10 — single param, readonly property.
BEZEL_TEST(codegen_primary_ctor_param_init_478, single_param_readonly_property_22_10) {
    // per §22.10 spec: `public readonly property Name: string := name;`
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Multiplier(factor: int) "
        "  public readonly property Factor: int := factor; "
        "  public method Apply(x: int): int => x * Factor; "
        "end Multiplier; "
        "class C public static method Main(): int "
        "  using var m := new Multiplier(6); "
        "  return m.Apply(7); "
        "end Main; end C;"), 42);
}
