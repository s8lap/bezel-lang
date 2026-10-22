// Spec-derived regression test for issue #7002.
//
// Spec §19.5 (Patterns, v0.340): "A typed binding pattern `var n: T` also matches
// only non-null values." §19.3 (v0.340): an untyped `var a` and a naked name
// bind whatever the slot holds, null included.
//
// THE DEFECT. A typed binding in a positional, named or case-payload slot bound
// the slot unconditionally, so `case Pair(null, 3) of Pair(var a: Object, var n)
// => "bind"` printed `bind` for `Pair(A: Object?, B: int)`. The same held for
// `string?`, a class `K?` and an `int?` slot. The top-level typed binding
// (`case x of var o: Object`) was already correct. The fix applies the non-null
// test in the slot path (Emitter::patternMatches).
//
// Rows are exact-stdout `runAot` / `runAotChecked` (sanitizers armed) rows;
// controls pin that an untyped binding still binds null and a non-null slot binds.
// Out of scope here: the slot's type test beyond null (#7003) and the value read
// of `var a: int` over a present `int?` slot (#7004).

#include <string>

#include "support/aot_support.hpp"  // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

}  // namespace

// The issue repro (#7002): `Pair(null, 3)` over `Pair(var a: Object, var n)` takes `_`.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, issue_repro_positional_object_slot) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method T(pr: Pair): string return (case pr of Pair(var a: Object, var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new Pair(null, 3))); return 0; end Main; end C;"),
        std::string("nobind\n"));
}

// Control: a non-null string in the slot still binds.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, non_null_object_slot_still_binds) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method T(pr: Pair): string return (case pr of Pair(var a: Object, var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new Pair(\"s\", 3))); return 0; end Main; end C;"),
        std::string("bind\n"));
}

// Control: an untyped `var a` and a naked `a` are not typed and still bind null (§19.3).
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, untyped_var_slot_still_binds_null) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method T(pr: Pair): string return (case pr of Pair(var a, var n) => \"bind\", _ => \"nobind\"); end T; public static method U(pr: Pair): string return (case pr of Pair(a, n) => \"bind\", _ => \"nobind\"); end U; public static method Main(): int Console.WriteLine(T(new Pair(null, 3))); Console.WriteLine(U(new Pair(null, 3))); return 0; end Main; end C;"),
        std::string("bind\nbind\n"));
}

// Named slot `Pair(A = var a: Object, B = var n)` (§19.3).
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, named_slot_object) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method T(pr: Pair): string return (case pr of Pair(A = var a: Object, B = var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new Pair(null, 3))); Console.WriteLine(T(new Pair(\"s\", 3))); return 0; end Main; end C;"),
        std::string("nobind\nbind\n"));
}

// A nested record pattern: the typed slot of the inner record.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, nested_record_slot) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); record Outer(P: Pair, Q: int); class C public static method T(o: Outer): string return (case o of Outer(Pair(var a: Object, var n), var q) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new Outer(new Pair(null, 3), 1))); Console.WriteLine(T(new Outer(new Pair(\"x\", 3), 1))); return 0; end Main; end C;"),
        std::string("nobind\nbind\n"));
}

// `string?` slot (the issue lists it).
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, string_slot) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record SP(A: string?, B: int); class C public static method T(pr: SP): string return (case pr of SP(var a: string, var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new SP(null, 3))); Console.WriteLine(T(new SP(\"s\", 3))); return 0; end Main; end C;"),
        std::string("nobind\nbind\n"));
}

// `K?` class slot (the issue lists it). plainOut: `new K()` passed straight into a
// record constructor leaks on base too, an independent defect.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, class_slot) {
    BEZEL_EXPECT_EQ(
        plainOut(
            "module M; public class K public method Hi(): string return \"hi\"; end Hi; end K; record KP(A: K?, B: int); class C public static method T(pr: KP): string return (case pr of KP(var a: K, var n) => a.Hi(), _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new KP(null, 3))); Console.WriteLine(T(new KP(new K(), 3))); return 0; end Main; end C;"),
        std::string("nobind\nhi\n"));
}

// `var a: int` over an `int?` slot: null does not match (§19.5).
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, value_slot_nullable_int) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record IP(A: int?, B: int); class C public static method T(pr: IP): string return (case pr of IP(var a: int, var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new IP(null, 3))); Console.WriteLine(T(new IP(7, 3))); return 0; end Main; end C;"),
        std::string("nobind\nbind\n"));
}

// A `when` guard arm: the typed null slot fails before the guard, the next arm runs.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, guard_falls_to_next_arm) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method T(pr: Pair): string return (case pr of Pair(var a: Object, var n) when n > 1 => \"g1\", Pair(var a, var n) when n > 1 => \"g2\", _ => \"other\"); end T; public static method Main(): int Console.WriteLine(T(new Pair(null, 3))); Console.WriteLine(T(new Pair(\"s\", 3))); Console.WriteLine(T(new Pair(\"s\", 0))); return 0; end Main; end C;"),
        std::string("g2\ng1\nother\n"));
}

// Arm order: a typed arm before an untyped arm sends null to the untyped arm.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, arm_order_typed_then_untyped) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method U(pr: Pair): string return (case pr of Pair(var a: Object, var n) => \"typed\", Pair(var a, var n) => \"untyped\", _ => \"other\"); end U; public static method Main(): int Console.WriteLine(U(new Pair(null, 4))); Console.WriteLine(U(new Pair(\"s\", 4))); return 0; end Main; end C;"),
        std::string("untyped\ntyped\n"));
}

// A generic record `Box<T>(X: T?, Y: int)` at T=string and T=Object.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, generic_slot_string) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Box<T>(X: T?, Y: int); class C public static method T(b: Box<string>): string return (case b of Box(var a: string, var n) => \"bind\", _ => \"nobind\"); end T; public static method U(b: Box<Object>): string return (case b of Box(var a: Object, var n) => \"bind\", _ => \"nobind\"); end U; public static method Main(): int Console.WriteLine(T(new Box<string>(null, 1))); Console.WriteLine(T(new Box<string>(\"s\", 1))); Console.WriteLine(U(new Box<Object>(null, 1))); Console.WriteLine(U(new Box<Object>(\"s\", 1))); return 0; end Main; end C;"),
        std::string("nobind\nbind\nnobind\nbind\n"));
}

// `X: T` at T=string?: the null is the substitution.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, generic_slot_substituted_nullable) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Box<T>(X: T, Y: int); class C public static method T(b: Box<string?>): string return (case b of Box(var a: string, var n) => \"bind\", _ => \"nobind\"); end T; public static method Main(): int Console.WriteLine(T(new Box<string?>(null, 1))); Console.WriteLine(T(new Box<string?>(\"s\", 1))); return 0; end Main; end C;"),
        std::string("nobind\nbind\n"));
}

// A choice payload slot: `Some(var a: Object)`.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, choice_payload_slot) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; choice Opt Some(V: Object?), None; end Opt; class C public static method T(o: Opt): string return (case o of Some(var a: Object) => \"bind\", Some(var b) => \"untyped\", None => \"none\"); end T; public static method Main(): int Console.WriteLine(T(Opt.Some(null))); Console.WriteLine(T(Opt.Some(\"s\"))); Console.WriteLine(T(Opt.None)); return 0; end Main; end C;"),
        std::string("untyped\nbind\nnone\n"));
}

// Two typed slots, each tested independently.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, two_typed_slots) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair2(A: Object?, B: string?); class C public static method T(pr: Pair2): string return (case pr of Pair2(var a: Object, var b: string) => \"both\", Pair2(var a: Object, var b) => \"a\", Pair2(var a, var b: string) => \"b\", _ => \"none\"); end T; public static method Main(): int Console.WriteLine(T(new Pair2(\"x\", \"y\"))); Console.WriteLine(T(new Pair2(\"x\", null))); Console.WriteLine(T(new Pair2(null, \"y\"))); Console.WriteLine(T(new Pair2(null, null))); return 0; end Main; end C;"),
        std::string("both\na\nb\nnone\n"));
}

// The statement form of `case`.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, case_statement_form) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method Main(): int var p: Pair := new Pair(null, 3); case p of Pair(var a: Object, var n) => Console.WriteLine(\"bind\"), Pair(var a, var n) => Console.WriteLine(\"untyped \" + n.ToString()) end case; return 0; end Main; end C;"),
        std::string("untyped 3\n"));
}

// Loop of 100 alternating null and non-null: 50 bind (not 100).
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, loop_alternating_null_count) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; record Pair(A: Object?, B: int); class C public static method Main(): int var i: int := 0; var cnt: int := 0; while i < 100 do var o: Object? := (if i mod 2 = 0 then null else \"v\"); var p: Pair := new Pair(o, i); cnt := cnt + (case p of Pair(var a: Object, var n) => 1, _ => 0); i := i + 1; end while; Console.WriteLine(cnt.ToString()); return 0; end Main; end C;"),
        std::string("50\n"));
}

// Loop over a class slot, 50 iterations: 25 bind, and the checked oracle sees no leak or double free.
BEZEL_TEST(codegen_pattern_typed_slot_binding_null_7002, loop_class_slot_owner) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; public class Res public method Hi(): string return \"hi\"; end Hi; end Res; record RP(A: Res?, B: int); class C public static method T(pr: RP): string return (case pr of RP(var a: Res, var n) => a.Hi(), _ => \"nobind\"); end T; public static method Main(): int var i: int := 0; var hits: int := 0; while i < 50 do var r: Res? := (if i mod 2 = 0 then null else new Res()); if T(new RP(r, i)) = \"hi\" then hits := hits + 1; end; i := i + 1; end while; Console.WriteLine(hits.ToString()); return 0; end Main; end C;"),
        std::string("25\n"));
}
