// Codegen JIT tests for bug #483 — `new Outer.Inner(args)` must construct
// the nested type Inner, not the outer type Outer.
//
// Authority: Grammar A.8 / §11.3. The `new` operator must parse and resolve
// a full QualifiedName as the constructed type; each dotted segment beyond
// the first is a nested-type lookup within the previously-resolved TypeSymbol.
//
// Tests verify:
//  1. `new Outer.Inner()` with field initializer → correct field value
//  2. `new Outer.Inner(args)` with explicit ctor taking args
//  3. `new Outer.Inner()._v` member access on the constructed nested object
//  4. Regression: `new Foo()` (single identifier) still works
//  5. Regression: `new Foo(1, 2)` with args still works

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §11.3 / Grammar A.8: `new Outer.Inner()` with a field initializer.
// Inner._v is initialized to 99; Main returns i._v which must be 99.
BEZEL_TEST(codegen_bug_483, new_nested_type_field_init_spec_11_3_A8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 99; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "class Main2 "
        "  public static method Main(): int "
        "    using var i := new Outer.Inner(); "
        "    return i._v; "
        "  end Main; "
        "end Main2;"), 99);
}

// §11.3 / §22.7: `new Outer.Inner(args)` with an explicit constructor.
// The ctor sets _v from the argument; Main returns i._v which must be 42.
BEZEL_TEST(codegen_bug_483, new_nested_type_with_ctor_args_spec_11_3_22_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 0; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Main(): int "
        "    using var i := new Outer.Inner(42); "
        "    return i._v; "
        "  end Main; "
        "end C;"), 42);
}

// §11.3: member access on the result of `new Outer.Inner()`.
// Construct then access — `new Outer.Inner()` is the object; `._v` follows.
BEZEL_TEST(codegen_bug_483, new_nested_type_then_member_access_spec_11_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 7; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Main(): int "
        "    using var i := new Outer.Inner(); "
        "    return i._v; "
        "  end Main; "
        "end C;"), 7);
}

// Regression: `new Foo()` (single-identifier, no nesting) must still work.
BEZEL_TEST(codegen_bug_483, new_single_identifier_regression_spec_A8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public _v: int := 55; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    return b._v; "
        "  end Main; "
        "end C;"), 55);
}

// Regression: `new Foo(1, 2)` with constructor args must still work.
BEZEL_TEST(codegen_bug_483, new_with_args_regression_spec_22_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Point "
        "  public _x: int := 0; "
        "  public _y: int := 0; "
        "  public constructor(x: int, y: int) _x := x; _y := y; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Point; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Point(20, 22); "
        "    return p._x + p._y; "
        "  end Main; "
        "end C;"), 42);
}
