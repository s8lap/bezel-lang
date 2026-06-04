// Codegen — `is` / `as` type tests whose operand is statically the root
// `Object` / `Object?` (§12.7, §17.4; the root type is §8.1). Issue #399.
//
// A type test inspects the operand's runtime type descriptor by loading the
// vtable pointer from the object header (a heap pointer) and walking the
// descriptor list. For that to lower correctly the operand must already be a
// pointer value. `Object` is the universal reference root (§8.1: "Every type
// derives from it … via their reference-type heap representation"), so an
// `Object` / `Object?` operand is a heap pointer and must lower to `ptr` —
// exactly like `string`. When it instead lowered to a non-pointer scalar, the
// type-test's runtime `ICmpEQ(operand, null-ptr)` compared a non-pointer
// against a pointer and aborted the compiler in LLVM verification.
//
// These cases exercise the broken path: `is` / `as` from an `Object?` operand,
// including the §8.3 idiom of a user `Equals(Object?)` override that downcasts
// with `as` and then compares fields. A positive control keeps the
// concrete-class operand path (`x as Animal`) working so the fix does not
// regress the already-passing CG19 tests.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A class P(x) overriding Equals(Object?) the §8.3 way: downcast `other` with
// `as P`, null-check, compare the `x` field. GetHashCode is required to
// accompany an Equals override (§8.3). Main body is spliced in.
int runWithP(const std::string& mainBody) {
    return run(
        "module M; "
        "class P "
        "  public x: int; "
        "  public constructor(v: int) this.x := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var p := other as P; "
        "    if p = null then return false; end; "
        "    return p.x = this.x; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.x; "
        "end P; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

// §8.3 / §17.4: `Equals(Object?)` downcasts its `Object?` parameter with `as P`
// then compares fields. Equal field values → Equals is true.
BEZEL_TEST(codegen_object_downcast, equals_override_equal_fields) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(7); using var b := new P(7); "
        "return (1 if a.Equals(b) else 0);"), 1);
}

// §8.3 / §17.4: differing field values → the downcast succeeds but the field
// comparison fails → Equals is false.
BEZEL_TEST(codegen_object_downcast, equals_override_differing_fields) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(7); using var b := new P(9); "
        "return (1 if a.Equals(b) else 0);"), 0);
}

// §17.4: `as` is null when the runtime type does not match. Passing `null`
// (an Object? null) → `null as P` is null → Equals false. Exercises the
// null-operand path of the type test on an Object? operand.
BEZEL_TEST(codegen_object_downcast, equals_override_null_argument) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(7); var n: Object? := null; "
        "return (1 if a.Equals(n) else 0);"), 0);
}

// §17.4: `as` from an Object? operand whose runtime type does not match the
// target yields null. Here Q is unrelated to P; `o as P` is null.
BEZEL_TEST(codegen_object_downcast, as_from_object_unrelated_type_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class P end P; "
        "class Q end Q; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new Q(); "
        "    var o: Object? := q; "
        "    return (1 if (o as P) is P else 0); "
        "  end Main; "
        "end C;"), 0);
}

// §12.7: `is T` on an Object? operand whose runtime type matches → true.
BEZEL_TEST(codegen_object_downcast, is_from_object_matching_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class P end P; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new P(); "
        "    var o: Object? := p; "
        "    return (1 if o is P else 0); "
        "  end Main; "
        "end C;"), 1);
}

// §12.7: `is T` on a null Object? operand is false (a type test on null fails;
// see §12.7 / §17.4, `as` yields null and `is` yields false).
BEZEL_TEST(codegen_object_downcast, is_from_null_object_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class P end P; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object? := null; "
        "    return (1 if o is P else 0); "
        "  end Main; "
        "end C;"), 0);
}

// §12.7: `is T` through an `Object?` PARAMETER (the exact slot shape that
// crashed: the operand is loaded directly from an Object?-typed parameter).
BEZEL_TEST(codegen_object_downcast, is_from_object_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class P end P; "
        "class C "
        "  public static method Check(o: Object?): int "
        "    => (1 if o is P else 0); "
        "  public static method Main(): int "
        "    using var p := new P(); return Check(p); "
        "  end Main; "
        "end C;"), 1);
}

// Positive control (must not regress CG19): a CONCRETE-class operand. `d as
// Animal` then `is Dog` holds. This is the path that already passed before the
// fix; it must keep passing.
BEZEL_TEST(codegen_object_downcast, as_from_concrete_class_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C "
        "  public static method Main(): int "
        "    var d := new Dog(); "
        "    return (1 if (d as Animal) is Dog else 0); "
        "  end Main; "
        "end C;"), 1);
}
