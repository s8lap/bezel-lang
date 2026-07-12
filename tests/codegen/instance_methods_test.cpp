// Codegen slice CG9 — instance methods and `this` (§22). A non-static
// method is emitted as an LLVM function taking the receiver as an
// implicit leading `this` pointer. Inside the body, `this`, an explicit
// `this.field`, and a bare implicit-`this` field/method reference all
// resolve against that pointer. An instance call `obj.M(args)` passes
// `obj` as the receiver. Observable via the int exit code.
//
// (Explicit constructors and ownership-driven disposal arrive later;
// these tests use the default `new T()` and trivially leak.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg9, instance_method_reads_field) {
    // Bare field reference inside the method is an implicit-`this` read.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 7; "
        "  public method Get(): int => _n; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); return c.Get(); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg9, explicit_this_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 13; "
        "  public method Get(): int => this._n; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); return c.Get(); "
        "end Main; end C;"), 13);
}

BEZEL_TEST(codegen_cg9, instance_method_with_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 10; "
        "  public method Add(x: int): int => _n + x; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); return c.Add(5); "
        "end Main; end C;"), 15);
}

BEZEL_TEST(codegen_cg9, instance_method_mutates_field) {
    // A block-body unit method assigns through implicit `this`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 0; "
        "  public method Set(v: int) _n := v; end Set; "
        "  public method Get(): int => _n; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); c.Set(42); return c.Get(); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg9, method_calls_sibling_via_implicit_this) {
    // `Get()` inside `Twice` is a bare call dispatched on the current `this`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 7; "
        "  public method Get(): int => _n; "
        "  public method Twice(): int => Get() + Get(); end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); return c.Twice(); "
        "end Main; end C;"), 14);
}

BEZEL_TEST(codegen_cg9, method_calls_sibling_via_explicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 9; "
        "  public method Get(): int => _n; "
        "  public method Doubled(): int => this.Get() * 2; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); return c.Doubled(); "
        "end Main; end C;"), 18);
}

BEZEL_TEST(codegen_cg9, two_instances_independent_via_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 0; "
        "  public method Set(v: int) _n := v; end Set; "
        "  public method Get(): int => _n; end Counter; "
        "class C public static method Main(): int "
        "  var a := new Counter(); var b := new Counter(); "
        "  a.Set(3); b.Set(40); return a.Get() + b.Get(); "
        "end Main; end C;"), 43);
}

BEZEL_TEST(codegen_cg9, method_combines_field_and_param) {
    // Field mutated, then read back through a computed method.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc public _sum: int := 0; "
        "  public method AddAll(a: int, b: int): int "
        "    _sum := _sum + a; _sum := _sum + b; return _sum; "
        "  end AddAll; end Acc; "
        "class C public static method Main(): int "
        "  var x := new Acc(); var _ := x.AddAll(10, 20); "
        "  return x.AddAll(5, 7); "
        "end Main; end C;"), 42);
}
