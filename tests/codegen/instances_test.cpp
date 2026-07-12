// Codegen slice CG8 — class instances: `new T()`, instance fields with
// initializers, field read/write (§11). Reference types lower to heap
// pointers (calloc'd, zero-initialized); field access is struct-GEP +
// load/store. Observable via the int exit code.
//
// (Explicit constructors, instance methods/`this`, and disposal/free
// arrive in later slices; trivial instances currently leak.)

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

BEZEL_TEST(codegen_cg8, field_initializer_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Point public _x: int := 5; public _y: int := 7; end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(); return p._x + p._y; "
        "end Main; end C;"), 12);
}

BEZEL_TEST(codegen_cg8, field_defaults_to_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); return b._v; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_cg8, field_assignment) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 1; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); b._v := 99; return b._v; "
        "end Main; end C;"), 99);
}

BEZEL_TEST(codegen_cg8, field_update_uses_old_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public _n: int := 10; end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(); c._n := c._n + 5; return c._n; "
        "end Main; end C;"), 15);
}

BEZEL_TEST(codegen_cg8, two_instances_independent) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 0; end Box; "
        "class C public static method Main(): int "
        "  var a := new Box(); var b := new Box(); "
        "  a._v := 3; b._v := 40; return a._v + b._v; "
        "end Main; end C;"), 43);
}

BEZEL_TEST(codegen_cg8, field_in_expression_and_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public _v: int := 100; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); "
        "  if b._v > 50 then return b._v * 2; end; return 0; "
        "end Main; end C;"), 200);
}
