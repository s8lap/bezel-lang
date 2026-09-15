// fix #4359 (v0.298, §35.9): case expression on nullable choice with choice-case
// arms (both bare `None` and `Some(var v)` destructuring patterns) produced
// "icmp eq i8, i32" IR verification failure.
//
// Two bugs were fixed in `patternMatches` in `LlvmBackend_Exprs2.cpp`:
//
// 1. Non-nullable choice handler (the `if (scrutTy && scrutTy->kind==Named &&
//    scrutTy->declaration->kind==Choice)` block) had no guard against nullable
//    scrutinees. For a nullable choice scrutinee, the LLVM value is the nullable
//    wrapper `{i8 hasValue; ChoiceStruct}`, not the bare choice struct.
//    `CreateExtractValue(scrut, {0})` therefore extracted the i8 hasValue flag,
//    and the subsequent `icmp eq i8, i32` was a verification failure.
//    Fix: add `&& !scrutTy->nullable` to the condition so nullable choices are
//    routed through the isNullableVal path below.
//
// 2. The isNullableVal path's DestructuringPattern handler passed `scrutTy=null`
//    on recursion, which skipped the (now-nullable-guarded) non-nullable choice
//    handler and fell through to the record handler, which emitted the same
//    "icmp eq i8, i32" error. Fix: when the nullable scrutinee is a Named
//    choice, construct a non-nullable Named TypeRef for the same choice
//    declaration and pass it to the recursive call.
//
// Additionally, a bare choice-case NamedPattern on a nullable choice scrutinee
// (e.g. `None` arm on `Maybe?`) had no handler in the isNullableVal path,
// falling through to `return nullptr` and leaving `case.end` unterminated.
// Fix: mirror the non-nullable choice NamedPattern handler (look up the case
// by name, extract the tag from innerVal, compare, AND with hasValue).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// `Some(var v)` arm on nullable choice: v should bind to the payload value.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, some_arm_binds_payload) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := Maybe.Some(42); "
        "  return (case m of "
        "    Some(var v) => v, "
        "    _ => -1); "
        "end Main; end C;"), 42);
}

// `None` bare case-name arm on nullable choice should match the None variant.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, none_arm_matches_none_variant) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := Maybe.None(); "
        "  return (case m of "
        "    Some(var v) => v, "
        "    None => 99, "
        "    _ => -1); "
        "end Main; end C;"), 99);
}

// `null` arm on nullable choice should match the null state.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, null_arm_matches_null_state) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := null; "
        "  return (case m of "
        "    Some(var v) => v, "
        "    null => 77, "
        "    _ => -1); "
        "end Main; end C;"), 77);
}

// Qualified case-name `Maybe.Some(var v)` arm on nullable choice.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, qualified_some_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := Maybe.Some(123); "
        "  return (case m of "
        "    Maybe.Some(var v) => v, "
        "    _ => -1); "
        "end Main; end C;"), 123);
}

// Bare `None` arm (no module qualifier) on nullable choice.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, bare_none_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := Maybe.None(); "
        "  return (case m of "
        "    Some(var v) => v, "
        "    None => 55, "
        "    null => 0, "
        "    _ => -1); "
        "end Main; end C;"), 55);
}

// Statement-position case over a nullable choice with choice-case arms.
BEZEL_TEST(case_nullable_choice_case_pattern_4359, statement_position_case) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Maybe Some(n: int), None; end Maybe; "
        "class C public static method Main(): int "
        "  var m: Maybe? := Maybe.Some(81); "
        "  case m of "
        "    Some(var v) => do return v; end, "
        "    None => do return 0; end, "
        "    null => do return -1; end, "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 81);
}
