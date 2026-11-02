// Spec-derived regression test for issue #8005 (campaign builtin-call-admission,
// lane C16): a built-in CONSTRUCTOR position whose parameter row carried NO
// TYPE OPINION (an all-zero `kinds` column) accepted ANY argument type —
// including a nullable one §7 should refuse.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// spec §5.1 "Implicit conversions": the implicit conversions are the identity,
// the widening table, and the CONSTANT narrowing — "An expression of integer
// type that is a compile-time constant implicitly converts to any integer type
// whose range contains its value".  Nothing else converts implicitly: "a
// `long` is never implicitly convertible to an `int` parameter", "a `string`
// is never implicitly convertible to a `bool` parameter", "a class instance is
// never implicitly convertible to a primitive parameter".
//
// spec §5.1, the never-implicit list: "The following are **never implicit**:
// ... - Between integers and `bool`."  and, first bullet: "Between signed and
// unsigned types of the same or narrower width (e.g. `int → uint`,
// `uint → int`, `short → ushort`) ... is never implicit."
//
// spec §7.3 "Assigning `null` to a non-nullable target": "Assigning `null` to
// a non-nullable target is a compile error."  spec §7.1: a nullable value
// does not implicitly convert to its non-nullable form.
//
// spec §22.5 (the parameter-NAME surface) and §17.7 (the built-in
// constructor surface): "For a standard-library builtin type ... the
// constructor surface is exactly what that document declares -- `new T(...)`
// resolves only against a declared constructor".
//
// stdlib §2.15 (Concrete serialization formats) declares, verbatim:
//
//     public class JsonSerializer: ISerializer
//       public constructor();
//       public constructor(options: JsonOptions);
//       ...
//     public record JsonOptions(
//       Indented: bool, IndentWidth: int, AllowTrailingCommas: bool,
//       IgnoreUnknownFields: bool, MaxDepth: int
//     );
//     public class BinaryDeserializer: IDeserializer
//       public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false);
//       ...
//
// stdlib §2.17 / spec §31.4 (the built-in exception types) declare, verbatim:
//
//     public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);
//
// stdlib §4.10 (`Set<T>`) declares `public constructor(capacity: int);`.
//
// --- THE DEFECT (MEASURED on r30-c1 = 73349f49) ------------------------------
//
// The ONE built-in parameter table (`src/common/BuiltinRegistry.cpp`'s
// `kBuiltinParams`) carries, per declared form, the parameter NAME (§22.5),
// the parameter TYPE (§5.1) and the parameter NULLABILITY (§7.3).  A row
// whose `kinds` column is ALL ZERO (`kBPTAny` — "this position has no type
// opinion") is SKIPPED by the §5.1 gate
// (`src/sema/ExprTypecheck_Exprs.cpp`, `ExprTC::checkBuiltinCtorArgTypes`:
// "a position whose kind is 0 in ANY form that binds carries no type opinion
// at all"), so NOTHING checked the argument's type there — and the §7.3
// nullability gate (the row's `nonNullable` mask) had nothing to read either.
//
// 94 of the table's 355 rows carried an all-zero `kinds` column on the base
// (the #7958 commit had already fixed 12 of the 106 the issue counted).
//
// THE PROBE: `new JsonSerializer(n)` over `var n: int?` COMPILED on the base.
//
// The rows below pin, each with the EXACT diagnostic:
//   * the issue's own probe — a NULLABLE value at a declared NON-nullable
//     position, refused with §7's nullability message;
//   * a `string` at a `bool` position (§5.1);
//   * a CLASS INSTANCE where a primitive is declared (§5.1);
//   * a `long` at a position where the standard library declares a `string`
//     (§5.1) — and, as the CONTROL for the issue's "a `long` at an `int`
//     position" family, the table's `int` CAPACITY (stdlib §4.10), which
//     #7958 closed;
//   * the LEGAL spellings, which must still compile and run.
//
// --- THE FIX (fix #8005) -----------------------------------------------------
//
// The `kinds` column and the `nonNullable` mask are transcribed for the
// population, and the §5.1 gate learns the whole vocabulary (it used to
// understand only `int`, `long` and `string`).  §7's nullability message is
// the one that fires for a nullable value at a non-nullable declared position
// — §5.1's conversion message is NOT the one.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A program over the serialization agents (stdlib §2.15) and the built-in
// exception types (spec §31.4).
std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Collections.Generic; import System.Text; "
           "import System.Serialization; "
           "class D public method Ping(): int return 1; end Ping; end D; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const char* needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, needle);
}

// §7's nullability message — the ONE the issue's own probe must draw.
const char* kNullableMessage =
    "is nullable but the target type is non-nullable (\xC2\xA7""7)";

// §5.1's conversion message — the house wording `checkAssignable` produces.
const char* kConversionMessage =
    "has a type that is not implicitly convertible to the target type "
    "(\xC2\xA7""5.1)";

}  // namespace

// --- THE ISSUE'S OWN PROBE: a NULLABLE value at a non-nullable position -----
//
// per spec §7.3 ("Assigning `null` to a non-nullable target is a compile
// error") + spec §7.1 (a nullable value does not implicitly convert to its
// non-nullable form) + stdlib §2.15 (`JsonSerializer(options: JsonOptions)` —
// `options` is declared WITHOUT `?`): `new JsonSerializer(n)` over
// `var n: int?` is a compile error, and the diagnostic is §7's NULLABILITY
// message — NOT §5.1's conversion message.
BEZEL_TEST(builtin_param_type_opinion_8005,
           issue_probe_nullable_options_refused) {
    auto r = runAot(prog(
        "    var n: int? := Id(5); "
        "    using var s := new JsonSerializer(n);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // §7's nullability message, naming the constructor.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `JsonSerializer` constructor is nullable but the "
           "target type is non-nullable (\xC2\xA7""7)"));
    // §5.1's CONVERSION message must NOT be the one: the argument's TYPE is
    // not the question — its NULLABILITY is.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kConversionMessage));
}

// per spec §7.3 ("Assigning `null` to a non-nullable target is a compile
// error") + stdlib §2.15: the `null` LITERAL at the same position is the
// same defect.
BEZEL_TEST(builtin_param_type_opinion_8005,
           null_literal_at_options_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    using var s := new JsonSerializer(null);"),
        "the argument to `JsonSerializer` constructor is nullable but the "
        "target type is non-nullable (\xC2\xA7""7)"));
}

// --- THE §5.1 FAMILIES: a wrong type at a declared position ----------------

// per spec §5.1 ("Between integers and `bool`" is never implicit) + stdlib
// §2.15 (`BinaryDeserializer(input: ReadOnlySpan<byte>,
// ignoreUnknownFields: bool := false)`): a `string` at the `bool` position is
// not implicitly convertible to it.
BEZEL_TEST(builtin_param_type_opinion_8005,
           string_at_bool_position_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    using var b: BinaryDeserializer := "
             "      new BinaryDeserializer(5, \"x\");"),
        "the `ignoreUnknownFields` argument of the `BinaryDeserializer` "
        "constructor has a type that is not implicitly convertible to the "
        "target type (\xC2\xA7""5.1)"));
}

// per spec §5.1 (a class instance is never implicitly convertible to a
// primitive) + spec §31.4 (`ExceptionInfo(TypeName: string, Message: string,
// StackTrace: string)`): a CLASS INSTANCE at a declared `string` position is
// refused.
BEZEL_TEST(builtin_param_type_opinion_8005,
           class_instance_at_primitive_position_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var d := new D(); "
             "    const e := new ExceptionInfo(d, \"M\", \"S\");"),
        "the `TypeName` argument of the `ExceptionInfo` constructor has a type "
        "that is not implicitly convertible to the target type (\xC2\xA7""5.1)"));
}

// per spec §5.1 (a `long` is never implicitly convertible to a `string`) +
// spec §31.4: a `long` at a declared `string` position is refused — the same
// family as the issue's "a `long` at an `int` position", at the position this
// lane's population carries.
BEZEL_TEST(builtin_param_type_opinion_8005,
           long_at_string_position_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    const e := new ExceptionInfo(5L, \"M\", \"S\");"),
        "the `TypeName` argument of the `ExceptionInfo` constructor has a type "
        "that is not implicitly convertible to the target type (\xC2\xA7""5.1)"));
}

// per spec §5.1 ("Between signed and unsigned types of the same or narrower
// width ... is never implicit") + stdlib §4.10 (`Set<T>(capacity: int)`):
// the table's `int` CAPACITY refuses a `long`.  (This position was closed by
// #7958, so it is green on the base — it is the CONTROL for the issue's
// "a `long` at an `int` position" family, and it must STAY green.)
BEZEL_TEST(builtin_param_type_opinion_8005, int_capacity_long_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var l: long := 5; "
             "    using var s: Set<int> := new Set<int>(l);"),
        "the `capacity` argument of the `Set` constructor has a type that is "
        "not implicitly convertible to the target type (\xC2\xA7""5.1)"));
}

// --- THE CONTROLS: every shape the fix must NOT touch -----------------------

BEZEL_TEST(builtin_param_type_opinion_8005, accept_controls) {
    // The legal `JsonSerializer` spellings still compile and run (stdlib
    // §2.15): the no-options constructor and the `JsonOptions` one.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var s0 := new JsonSerializer(); "
        "    using var s1 := new JsonSerializer(default(JsonOptions)); "
        "    using var s2 := new JsonSerializer("
        "      new JsonOptions(true, 2, false, false, 3)); "
        "    Console.WriteLine(1); ")),
        "1");
    // `ExceptionInfo`'s three `string` components are still accepted.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    const e := new ExceptionInfo(\"T\", \"M\", \"S\"); "
        "    Console.WriteLine(e.Message); ")),
        "M");
    // The `bool` component of `BinaryDeserializer` is still accepted (the
    // `input` component is a `ReadOnlySpan<byte>` / `ReadOnlyArraySpan<byte>`
    // — stdlib §2.15 — and is written as the latter here, over a real
    // `Array<byte>`).
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var bytes: Array<byte> := [1, 2, 3]; "
        "    using var b: BinaryDeserializer := new BinaryDeserializer("
        "      new ReadOnlyArraySpan<byte>(bytes, 0, 3), true); "
        "    Console.WriteLine(1); ")),
        "1");
    // A §7.4-NARROWED nullable is legal at a non-nullable position (§22.6:
    // "a read of a binding §7.4 has narrowed non-null is of its non-nullable
    // type") and still runs.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var n: int? := Id(5); "
        "    if n is not null then "
        "      using var s := new JsonSerializer(default(JsonOptions)); "
        "    end if; "
        "    Console.WriteLine(1); ")),
        "1");
    // A NULLABLE parameter is still admitted where the standard library
    // declares one nullable: stdlib §2.4's `Environment.Set(name: string,
// value: string?)` — `value = null removes the variable`.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Environment.Set(\"BEZEL_TEST_8005\", null); "
        "    Console.WriteLine(1); ")),
        "1");
}
