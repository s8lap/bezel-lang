// Codegen — discovery sweep #7: a `case` type-test pattern whose type is
// incompatible with the scrutinee crashed or returned a wrong value, because the
// pattern lowering called emitIsA / returned an unconditional match without
// checking the scrutinee kind:
//   - a class type-test on an `int` scrutinee → emitIsA compared i32 to a ptr →
//     LLVM SIGABRT (compiler crash).
//   - a class type-test on a `string` scrutinee → emitIsA loaded a vtable from a
//     non-object → SIGSEGV at runtime.
//   - a primitive type pattern (`string`) on an `int` scrutinee → matched
//     unconditionally (wrong value).
//
// THE FIX: a class/interface type-test only matches a heap object with a
// vtable/descriptor (a class, interface, or Object) — otherwise it returns false
// (never matches). A primitive type pattern against a different concrete
// primitive also returns false. Such an arm is dead and falls through to the
// wildcard. §19.3 / §19.5.
//
// UPDATE (#5188, §19.3 v0.311/v0.313, Appendix C.1): a class/interface/`string`/
// `Object` type pattern over a VALUE-type scrutinee (primitive, record, enum,
// choice, inline array, unit, or their nullable forms) is now a COMPILE ERROR
// ("a value-type scrutinee can never match a reference-typed type pattern"),
// not a dead arm. Two rows below that pinned the old accept-and-fall-through
// behaviour over an `int` scrutinee are flipped to expect that reject:
// class_pattern_on_int_never_matches and primitive_pattern_mismatch_no_match.
// The string-scrutinee row (a reference type) and the Object-scrutinee control
// are unchanged.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// class type-test on an int scrutinee (was a bezelc SIGABRT; §19.3 v0.313 /
// C.1, #5188: now a compile-time reject).
BEZEL_TEST(case_incompatible_type_pattern, class_pattern_on_int_never_matches) {
    auto r = runAot(
        "module M; "
        "class Box private _x: int := 0; destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  return (case n of  Box => 1, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "value-type scrutinee can never match a reference-typed type pattern"));
}

// class type-test on a string scrutinee (was a runtime SIGSEGV) → never matches.
BEZEL_TEST(case_incompatible_type_pattern, class_pattern_on_string_never_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal private _x: int := 0; destructor end destructor; end Animal; "
        "class C public static method Main(): int "
        "  var s: string := \"hello\"; "
        "  return (case s of  Animal => 1, _ => 0); "  // 0
        "end Main; end C;"), 0);
}

// `string` type pattern over an int scrutinee (was a wrong value; §19.3 v0.313
// / C.1, #5188: `string` is a reference-typed pattern, so a compile-time reject).
BEZEL_TEST(case_incompatible_type_pattern, primitive_pattern_mismatch_no_match) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var x: int := 42; "
        "  return (case x of  string => 1, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "value-type scrutinee can never match a reference-typed type pattern"));
}

// Control: a VALID class type-test on an Object scrutinee that holds that class
// still matches (the guard must not over-restrict).
BEZEL_TEST(case_incompatible_type_pattern, valid_object_to_class_test_still_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public v: int; "
        "  public constructor(x: int) v:=x; end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(7); "
        "  var o: Object := b; "
        "  return (case o of  Box => 1, _ => 0); "  // 1
        "end Main; end C;"), 1);
}
