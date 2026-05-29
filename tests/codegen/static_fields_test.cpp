// Codegen slice CG12 — static fields (§11/§22). A `static` field is
// emitted as an internal global variable shared by all instances and
// the class's static methods. Reads/writes of a static field (referenced
// bare within the declaring class) load/store that global; a literal
// initializer becomes the global's constant initial value. Observable via
// the int exit code.
//
// (Cross-class `Type.field` static access and non-literal static
// initializers — which the spec runs in a static constructor, §22 — are
// deferred; tests reference statics bare within the declaring class.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& members) {
    auto r = runJit(std::string("module M; class C ") + members + " end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg12, static_field_literal_initializer) {
    BEZEL_EXPECT_EQ(run(
        "public static _v: int := 7; "
        "public static method Main(): int => _v;"), 7);
}

BEZEL_TEST(codegen_cg12, static_field_defaults_to_zero) {
    BEZEL_EXPECT_EQ(run(
        "public static _v: int; "
        "public static method Main(): int => _v;"), 0);
}

BEZEL_TEST(codegen_cg12, static_field_assignment) {
    BEZEL_EXPECT_EQ(run(
        "public static _v: int := 1; "
        "public static method Main(): int "
        "  _v := 42; return _v; "
        "end Main;"), 42);
}

BEZEL_TEST(codegen_cg12, static_field_shared_across_calls) {
    // Each Bump() mutates the one shared global; the count persists.
    BEZEL_EXPECT_EQ(run(
        "public static _count: int := 0; "
        "public static method Bump() _count := _count + 1; end Bump; "
        "public static method Main(): int "
        "  Bump(); Bump(); Bump(); return _count; "
        "end Main;"), 3);
}

BEZEL_TEST(codegen_cg12, static_field_accumulates_in_loop) {
    BEZEL_EXPECT_EQ(run(
        "public static _sum: int := 0; "
        "public static method Add(n: int) _sum := _sum + n; end Add; "
        "public static method Main(): int "
        "  var i := 1; "
        "  while i <= 5 do Add(i); i := i + 1; end; "
        "  return _sum; "
        "end Main;"), 15);
}

BEZEL_TEST(codegen_cg12, static_bool_field) {
    BEZEL_EXPECT_EQ(run(
        "public static _flag: bool := true; "
        "public static method Main(): int "
        "  if _flag then return 1; end; return 0; "
        "end Main;"), 1);
}

BEZEL_TEST(codegen_cg12, two_static_fields_independent) {
    BEZEL_EXPECT_EQ(run(
        "public static _a: int := 10; "
        "public static _b: int := 20; "
        "public static method Main(): int "
        "  _a := _a + 5; return _a + _b; "
        "end Main;"), 35);
}
