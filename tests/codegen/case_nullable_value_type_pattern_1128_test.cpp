// fix #1128 / §19.5: a bare VALUE-type type-test pattern (`int`, an enum, a
// record) over a NULLABLE VALUE-type scrutinee (`int?`, `Color?`, `Pt?`).
//
// §19.5 (v0.153, Spec L3097): "A bare type pattern T matches only non-null
// values." A nullable value type is the unboxed tagged `{i8 hasValue; T value}`
// struct (NOT a pointer), so `patternMatches` enters its `isNullableVal` branch.
// That branch handled null/constant/binding/range/relop/destructure patterns but
// had no handler for a bare TYPE pattern (`int` → TypePattern; `Color`/`Pt` → a
// NamedPattern whose name is the scrutinee's own type) — both fell through to the
// deferred `return nullptr`. The case-lowering then BAILED, leaving the merge
// block (`case.end`) unterminated → an expr-position IR-verification crash, and
// in statement position the arm silently never matched and short-circuited every
// later arm (WRONG_VALUE). The fix matches a present value (hasValue) when the
// pattern type is the scrutinee's underlying value type.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5: a present `int?` (= 7) matches the bare `int` pattern — the primary
// expr-position repro that crashed IR verification (unterminated `case.end`). → 1
BEZEL_TEST(codegen_case_nullable_value_type_pattern_1128, expr_int_matches_present_nullable_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 7; "
        "    var r := case x of "
        "      int => 1, "
        "      _   => 2 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 1);
}

// §19.5: the statement-position counterpart — the bare `int` arm must FIRE for a
// present `int?` (it silently never matched before, leaving `acc` at 99). → 1
BEZEL_TEST(codegen_case_nullable_value_type_pattern_1128, stmt_int_arm_fires_for_present_nullable_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 7; "
        "    var acc := 99; "
        "    case x of "
        "      int => acc := 1 "
        "    end; "
        "    return acc; "
        "  end Main; "
        "end C;"), 1);
}

// §19.5: a NULL `int?` must NOT match the bare `int` pattern; the `_` arm fires
// (the bare type pattern matches only non-null values). → 2
BEZEL_TEST(codegen_case_nullable_value_type_pattern_1128, null_nullable_int_does_not_match_type_pattern) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := null; "
        "    var r := case x of "
        "      int => 1, "
        "      _   => 2 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 2);
}

// §19.5 / §10: a present nullable ENUM (`Color?` = Color.Green) matches the bare
// enum-type pattern `Color` (a sealed value type — its own type, non-null). → 5
BEZEL_TEST(codegen_case_nullable_value_type_pattern_1128, nullable_enum_matches_bare_enum_type) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var x: Color? := Color.Green; "
        "    var r := case x of "
        "      Color => 5, "
        "      _     => 9 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 5);
}

// §19.5 / §9.6: a present nullable RECORD (`Pt?`) matches the bare record-type
// pattern `Pt` (a sealed value type — its own type, non-null). → 7
BEZEL_TEST(codegen_case_nullable_value_type_pattern_1128, nullable_record_matches_bare_record_type) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt? := new Pt(3, 4); "
        "    var r := case p of "
        "      Pt => 7, "
        "      _  => 8 "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 7);
}
