// Regression tests for #1077 (§8.5 / §18.5): interpolating a class- or
// interface-typed value that uses the DEFAULT ToString (no explicit override).
// Per §8.5 the default ToString of a class/enum is its type name. The
// interpolation lowering walked the class chain for an explicit ToString and,
// finding none, treated the class POINTER as a raw bz_string (`pv = ev`) — so
// `$"{obj}"` produced an empty/garbage string. (emitValueToString also lacked
// an Interface branch.) The fix routes the no-override class case through
// emitValueToString and adds an Interface branch dispatching the dynamic
// (vtable) ToString. The Object static type and an explicit override already
// worked and must keep working.
//
// Each program returns the interpolated string's Length so the exit code
// encodes the rendered text (e.g. "[MyClass]" → 9).
//
// runJit returns the raw exit code (0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// A class with no ToString override interpolates as its type name ("MyClass").
BEZEL_TEST(interp_default_tostring_1077, concrete_class_default_is_type_name) {
    BEZEL_EXPECT_EQ(run(
        "module M; class MyClass end MyClass; "
        "class C public static method Main(): int "
        "  using var obj := new MyClass(); "
        "  return $\"[{obj}]\".Length; "          // "[MyClass]" = 9
        "end Main; end C;"), 9);
}

// The Object static type renders the dynamic type name (already worked).
BEZEL_TEST(interp_default_tostring_1077, object_static_type_dynamic_name) {
    BEZEL_EXPECT_EQ(run(
        "module M; class MyClass end MyClass; "
        "class C public static method Main(): int "
        "  using var obj := new MyClass(); "
        "  var o: Object := obj; "
        "  return $\"[{o}]\".Length; "             // "[MyClass]" = 9
        "end Main; end C;"), 9);
}

// An INTERFACE static type renders the runtime type name (was "" before #1077).
BEZEL_TEST(interp_default_tostring_1077, interface_static_type_runtime_name) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape end IShape; class Sq: IShape end Sq; "
        "class C public static method Main(): int "
        "  using var sq := new Sq(); "
        "  var s: IShape := sq; "
        "  return $\"[{s}]\".Length; "             // "[Sq]" = 4
        "end Main; end C;"), 4);
}

// An explicit ToString override is still used (not the type name).
BEZEL_TEST(interp_default_tostring_1077, explicit_override_used) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Named public override method ToString(): string => \"XY\"; end Named; "
        "class C public static method Main(): int "
        "  using var n := new Named(); "
        "  return $\"<{n}>\".Length; "             // "<XY>" = 4
        "end Main; end C;"), 4);
}

// Dynamic dispatch through an interface static type uses the override.
BEZEL_TEST(interp_default_tostring_1077, interface_dispatches_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape end IShape; "
        "class Sq: IShape public override method ToString(): string => \"AB\"; end Sq; "
        "class C public static method Main(): int "
        "  using var sq := new Sq(); "
        "  var s: IShape := sq; "
        "  return $\"{s}\".Length; "               // "AB" = 2
        "end Main; end C;"), 2);
}

// Dynamic dispatch through a base-class static type uses the most-derived
// override.
BEZEL_TEST(interp_default_tostring_1077, base_static_type_dispatches_derived) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public override method ToString(): string => \"B\"; end Base; "
        "class Der: Base public override method ToString(): string => \"DER\"; end Der; "
        "class C public static method Main(): int "
        "  using var d := new Der(); "
        "  var b: Base := d; "
        "  return $\"{b}\".Length; "               // "DER" = 3
        "end Main; end C;"), 3);
}

// ── #1118: interpolating a generic type-parameter operand `$"{x}"` (x: T) ─────
//
// A generic-T operand is a declaration-less Named TypeRef; the interp lowering
// fell to `pv = ev`, reading the heap object as a bz_string → "". The fix routes
// it through emitValueToString, which resolves T via the generic substitution
// (and otherwise dispatches the dynamic ToString).

// T bound to a class with a ToString override dispatches virtually → "ABCD".
BEZEL_TEST(interp_default_tostring_1077, generic_param_class_override_1118) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W public override method ToString(): string => \"ABCD\"; end W; "
        "class C "
        "  public method Show<T>(x: T): int => ($\"{x}\").Length; "
        "  public static method Main(): int "
        "    using var c := new C(); using var w := new W(); "
        "    return c.Show<W>(w); "                // "ABCD" = 4
        "  end Main; end C;"), 4);
}

// Surrounding literals survive and the value renders: `$"[{x}]"` → "[ABCD]".
BEZEL_TEST(interp_default_tostring_1077, generic_param_with_literals_1118) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W public override method ToString(): string => \"ABCD\"; end W; "
        "class C "
        "  public method Show<T>(x: T): int => ($\"[{x}]\").Length; "
        "  public static method Main(): int "
        "    using var c := new C(); using var w := new W(); "
        "    return c.Show<W>(w); "                // "[ABCD]" = 6
        "  end Main; end C;"), 6);
}

// T bound to a class with NO override interpolates as its type name ("Plain").
BEZEL_TEST(interp_default_tostring_1077, generic_param_class_default_1118) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Plain public v: int := 0; end Plain; "
        "class C "
        "  public method Show<T>(x: T): int => ($\"{x}\").Length; "
        "  public static method Main(): int "
        "    using var c := new C(); using var p := new Plain(); "
        "    return c.Show<Plain>(p); "            // "Plain" = 5
        "  end Main; end C;"), 5);
}

// T bound to a value type (int) resolves through the substitution → "42".
BEZEL_TEST(interp_default_tostring_1077, generic_param_value_type_1118) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public method Show<T>(x: T): int => ($\"{x}\").Length; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Show<int>(42); "             // "42" = 2
        "  end Main; end C;"), 2);
}
