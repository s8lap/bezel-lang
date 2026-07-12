// Codegen — discovery sweep #8: an extension computed property on a PRIMITIVE
// value type crashed bezelc (LLVM ICmpInst assertion / IR_FAIL). The property-
// access lowering spilled a non-pointer receiver to a stack pointer (so a record
// getter's `this.field` works), but an extension property on a scalar primitive
// uses `this` as the value (i32/i1) — spilling it and using the pointer as `this`
// made the getter read `this` at the wrong type (`this = 0` → `icmp ptr, i32`).
// THE FIX: spill only non-pointer AGGREGATE (record) receivers; a scalar
// receiver (like an enum) is left unspilled. §26 extensions.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
namespace { int run(const std::string& p){ auto r=runAot(p); return r.ran?r.exitCode:-999; } }

// The verified repro: `extension int property IsZero => this = 0`.
BEZEL_TEST(extension_property_primitive, int_bool_scalar_extension_properties) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int public property Doubled: int => this * 2; end; "
        "extension int public property IsZero: bool => this = 0; end; "
        "extension bool public property Flipped: bool => not this; end; "
        "class C public static method Main(): int "
        "  var x: int := 21; var z: int := 0; var b: bool := false; "
        "  return x.Doubled + (10 if z.IsZero else 0) + (1 if b.Flipped else 0); "  // 42+10+1=53
        "end Main; end C;"), 53);
}

// Control: a RECORD extension property (which DOES need the spill) still works.
BEZEL_TEST(extension_property_primitive, record_extension_property_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "extension Pt public property Sum: int => X + Y; end; "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return p.Sum; "  // 7
        "end Main; end C;"), 7);
}
