// Codegen + sema — spec §12.7 / §17.4, issue #3410: a type test whose target is
// STATICALLY UNRELATED to the operand's type is LEGAL, and answers at run time.
//
// WHY THIS FILE EXISTS. #3410 asserts that LINQ `OfType<U>()` "accepts a type
// argument `U` with no relatedness check against the element type", and that
// "an `OfType<U>` where `U` is unrelated to `T` should presumably be rejected",
// on the stated ground that "spec §12.7's `is` machinery rejects statically-
// impossible tests in other positions".
//
// That ground is FALSE in both the specification and the implementation, and
// these rows are the measurement. They are the NEGATIVE CONTROLS for any future
// #3410 fix: a fix that makes an unrelated `U` a compile error at `OfType` must
// not reach the shared §12.7 machinery, or these rows go red.
//
// WHAT THE SPEC ACTUALLY SAYS (v0.283).
//
// §12.7 *Type tests: `is`, `is not`, `as`* states the operator's contract with
// no relatedness precondition whatsoever:
//
//   "`is T` tests whether a value is an instance of `T` and produces `bool`."
//
//   "`as` returns the value cast to the target type, or `null` if the cast
//    fails. Its result is always nullable."
//
// A "statically impossible" test is simply one whose answer is knowable early;
// the spec gives it the same status as any other, and gives `as` a `null` result
// for exactly the case where the cast does not hold.
//
// §17.4 *The `is`, `is not`, and `as` operators* is the section that enumerates
// which `as` targets ARE compile errors, and it is a CLOSED list of two — both
// rejected for a reason that has nothing to do with relatedness:
//
//   "**Degenerate value-type targets.** `as` requires its result type `T?` to
//    exist and to be informative. An **inline-array target** (any form —
//    `[N]T`, `[N]T?`, and the owning forms, which cannot even be boxed, §8.6)
//    is a **compile error naming this rule** ... The **unit type** is likewise
//    a compile error as an `as` target"
//
// An unrelated class, an unimplemented interface, and a mismatched value type
// are not on that list. There is no rule in the specification that rejects a
// statically-impossible type test, in any position.
//
// AXIS GRID (each row is one cell; every negative cell is paired with a
// positive control so a "false" answer cannot be mistaken for a broken test):
//
//   reference / unrelated class      -> legal, false        (unrelated_class_*)
//   reference / unrelated class, `as`-> legal, null         (as_unrelated_class_*)
//   reference / unimplemented iface  -> legal, false        (unimplemented_interface_*)
//   reference / real downcast        -> legal, TRUE         (POSITIVE CONTROL)
//   value / mismatched primitive     -> legal, false        (value_unrelated_*)
//   boxed Object / matching value    -> legal, TRUE         (POSITIVE CONTROL)
//   boxed Object / mismatched value  -> legal, false        (boxed_mismatched_*)
//
// NOT COVERED HERE, deliberately: the `OfType<U>()` defect itself. #3410's
// OBSERVATION (no relatedness check) is real, but the defect it exposes is
// larger and different in kind from the one filed — the operator performs no
// runtime type test AT ALL and is a type confusion for every `U` other than the
// element type or a supertype of it, including its own canonical downcast use.
// That is tracked separately; pinning today's behaviour here would pin a bug.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// Two unrelated CLASSES — neither derives from the other. Single inheritance
// (§12) makes them provably disjoint: no value is ever both.
const char* kUnrelatedClasses =
    "class Dog3410 "
    "  public property Age: int; "
    "  public constructor(a: int) Age := a; end constructor; "
    "end Dog3410; "
    "class Widget3410 "
    "  public property Size: int; "
    "  public constructor(a: int) Size := a; end constructor; "
    "end Widget3410; ";

} // namespace

// =====================================================================
// Reference operand, unrelated class target — §12.7.
// =====================================================================

// §12.7: `d is Widget3410` on a `Dog3410` is a well-formed test that produces
// `bool`. It compiles, and the answer is false. If it were REJECTED — the
// remedy #3410 proposes for the corresponding `OfType` shape — this row would
// report "<did-not-run>".
BEZEL_TEST(codegen_type_test_unrelated_3410,
           unrelated_class_is_test_compiles_and_answers_false_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; ") + kUnrelatedClasses +
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog3410(1); "
        "    if d is Widget3410 then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("no"));
}

// §17.4: `d as Widget3410` is not one of the two degenerate targets §17.4
// rejects (inline array, unit), so it compiles; §12.7 gives it `null` because
// "the cast fails".
BEZEL_TEST(codegen_type_test_unrelated_3410,
           as_unrelated_class_compiles_and_yields_null_spec_17_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; ") + kUnrelatedClasses +
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog3410(1); "
        "    var w := d as Widget3410; "
        "    if w = null then Console.WriteLine(\"null\"); "
        "    else Console.WriteLine(\"nonnull\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("null"));
}

// §12.7: an interface NO type in the program implements is still a legal test
// target. (A later-declared subclass could implement it, so the test is not even
// statically decidable in general — which is the deeper reason the spec states
// no relatedness precondition.)
BEZEL_TEST(codegen_type_test_unrelated_3410,
           unimplemented_interface_is_test_compiles_and_answers_false_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "interface IUnused3410 "
        "  method Nothing(): int; "
        "end IUnused3410; "
        "class Dog3410 "
        "  public property Age: int; "
        "  public constructor(a: int) Age := a; end constructor; "
        "end Dog3410; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog3410(1); "
        "    if d is IUnused3410 then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("no"));
}

// POSITIVE CONTROL for the three rows above. A genuine downcast — an
// `Animal3410`-typed binding holding a `Dog3410` — answers TRUE. Without this
// row a compiler that answered `false` for every reference test would pass the
// whole negative set.
BEZEL_TEST(codegen_type_test_unrelated_3410,
           control_real_downcast_answers_true_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Animal3410 "
        "  public property Age: int; "
        "  public constructor(a: int) Age := a; end constructor; "
        "end Animal3410; "
        "class Dog3410: Animal3410 "
        "  public constructor(a: int) base(a); end constructor; "
        "end Dog3410; "
        "class C "
        "  public static method Main(): int "
        "    using var d: Animal3410 := new Dog3410(1); "
        "    if d is Dog3410 then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("yes"));
}

// =====================================================================
// Value operand, mismatched value target — §12.7.
// =====================================================================

// §12.7: `x is string` on an `int` is legal and false. This is the cell #3410's
// `OfType` analogue (`Array<int>.OfType<string>()`) sits in — and the language's
// answer here is a value, not a diagnostic.
BEZEL_TEST(codegen_type_test_unrelated_3410,
           value_unrelated_primitive_is_test_answers_false_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int := 5; "
        "    if x is string then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("no"));
}

// §12.7 + §8.6: same shape one type wider — an `int` that reached an `Object`
// location is a box, and `o is double` is legal and false.
BEZEL_TEST(codegen_type_test_unrelated_3410,
           boxed_mismatched_value_is_test_answers_false_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    if o is double then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("no"));
}

// POSITIVE CONTROL for the two value rows. The SAME boxed operand answers TRUE
// for its own type, so the `false`s above are answers and not a dropped test.
// (§12.7's own machinery reports a DROPPED test as a warning, not as `false`
// silently — but a control is cheaper than trusting that.)
BEZEL_TEST(codegen_type_test_unrelated_3410,
           control_boxed_matching_value_is_test_answers_true_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    if o is int then Console.WriteLine(\"yes\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("yes"));
}
