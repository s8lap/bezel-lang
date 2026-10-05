// Codegen slice CG12 — static fields (§11/§22). A `static` field is
// emitted as an internal global variable shared by all instances and
// the class's static methods. Reads/writes of a static field (referenced
// bare within the declaring class) load/store that global; a literal
// initializer becomes the global's constant initial value. Observable via
// the int exit code.
//
// Cross-class / qualified `Type.field` static access reads and writes the
// same backing global (#213). Non-literal static initializers — which the
// spec runs in a static constructor, §22 — remain deferred.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& members) {
    auto r = runAot(std::string("module M; class C ") + members + " end C;");
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

// #213: a qualified static-field access `Type.Field` reads and writes the
// same backing global (it has no instance slot, so the read path must
// consult the static global directly — previously the read returned the
// initializer value and the write was dropped).
namespace {
int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg12, qualified_static_write_then_read) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N public static Count: int := 0; end N; "
        "class C public static method Main(): int "
        "  N.Count := 5; return N.Count; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg12, instance_method_mutates_own_static) {
    // A bare static reference inside an instance method targets the static
    // backing global; the mutation is visible via `Type.Field` afterwards.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public static Count: int := 0; "
        "  public method Bump() Count := Count + 1; end Bump; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.Bump(); n.Bump(); n.Bump(); return N.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_cg12, cross_class_static_read) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N public static V: int := 42; end N; "
        "class C public static method Main(): int => N.V; "
        "end C;"), 42);
}

// R40 / #743 / §15.4 + §22.13: a static field initialized with a COLLECTION
// LITERAL must materialize the heap collection in the static-init path and store
// it into the global. Previously lowerExpr returned null for the collection
// literal, so the store was skipped and the global stayed null → null deref on
// first access (`Type.All.Length` → SIGSEGV).
BEZEL_TEST(codegen_cg12, static_array_literal_field_743) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class H public static readonly Nums: Array<int> := [10, 20, 30]; end H; "
        "class C public static method Main(): int "
        "  Console.WriteLine(H.Nums.Count * 100 + H.Nums.Get(0) + H.Nums.Get(2)); return 0; "  // 3*100+10+30 = 340
        "end Main; end C;"), "340");
}

// On an ENUM (the original repro): a static FrozenArray of the enum's
// values — the §10.4 v0.256 spelling (`FrozenArray<Color>.From([...])`).
// Fix #3249: FrozenArray<T> carries the immutable fact (stdlib §4.19), so
// this `static readonly` of it is the sanctioned spelling the spec's
// §10.4 example names (the pre-#3249 `Array<Color>` shape would draw
// §22.12 rule 4's sharing warning; FrozenArray<Color> does not, by being
// in rule 4's warning-suppression set).
BEZEL_TEST(codegen_cg12, enum_static_frozen_array_literal_field_3249) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color Red, Green, Blue; "
        "  public static readonly All: FrozenArray<Color> := "
        "      FrozenArray<Color>.From([Red, Green, Blue]); "
        "end Color; "
        "class C public static method Main(): int return Color.All.Length; end Main; "  // 3
        "end C;"), 3);
}

// A static field initialized with a DICTIONARY literal.
BEZEL_TEST(codegen_cg12, static_dict_literal_field_743) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class H public static readonly M2: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; end H; "
        "class C public static method Main(): int "
        "  return H.M2.Count * 10 + H.M2[\"a\"] + H.M2[\"b\"]; "  // 2*10+1+2 = 23
        "end Main; end C;"), 23);
}
