// Codegen slice CG10 — explicit instance constructors (§22.7). A
// non-static constructor is emitted as a `void(this, params...)`
// function; `new T(args)` calloc's the instance, runs the field
// initializers, then calls the constructor with the lowered arguments.
// The body assigns through `this` like any instance method. Observable
// via the int exit code.
//
// (Constructor overloads and chained `base(...)`/`this(...)` calls are
// later slices — sema also defers the overloaded case, so at most one
// constructor reaches codegen here. Instances still leak until disposal
// lowering lands.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg10, ctor_initializes_field_from_arg) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Get(): int => _v; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(42); return b.Get(); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg10, ctor_with_multiple_args) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Point public _x: int := 0; public _y: int := 0; "
        "  public constructor(x: int, y: int) _x := x; _y := y; "
        "  end constructor; end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(30, 12); return p._x + p._y; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg10, ctor_runs_after_field_initializers) {
    // The field initializer sets _v := 5 first; the constructor adds to it,
    // so it observes the initialized value.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 5; "
        "  public constructor(d: int) _v := _v + d; end constructor; "
        "  public method Get(): int => _v; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(37); return b.Get(); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg10, ctor_with_explicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 0; "
        "  public constructor(v: int) this._v := v; end constructor; "
        "  public method Get(): int => this._v; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(99); return b.Get(); "
        "end Main; end C;"), 99);
}

BEZEL_TEST(codegen_cg10, ctor_computes_from_args) {
    // Constructor body does arithmetic across its parameters and a field.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Rect public _area: int := 0; "
        "  public constructor(w: int, h: int) _area := w * h; "
        "  end constructor; "
        "  public method Area(): int => _area; end Rect; "
        "class C public static method Main(): int "
        "  var r := new Rect(6, 7); return r.Area(); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg10, two_instances_independent_ctors) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Get(): int => _v; end Box; "
        "class C public static method Main(): int "
        "  var a := new Box(40); var b := new Box(2); "
        "  return a.Get() + b.Get(); "
        "end Main; end C;"), 42);
}
