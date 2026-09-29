// fix #5100 (§9.3 / §9.8, v0.307): a record body constructor that
// EXACTLY duplicates the primary constructor — same arity, same case-sensitive
// names, same resolved types, same §22.6 overload modifiers, same default
// presence — must be REJECTED.
//
// §9.3 states: "The body may not declare a constructor with the same
// parameter list as the auto-generated primary constructor." Every candidate
// reading of #4775 Q1 (types-only, case-insensitive names, case-sensitive
// names) rejects the exact-duplicate shape. §9.8 step 2 requires a
// combined-form record that declares any explicit constructor to declare one
// binding exactly its components by case-INSENSITIVE name — so
// `constructor(x: int)` beside `R(X: int)` is NOT an exact duplicate and
// is governed by §9.8, not §9.3.
//
// The fix adds the check to `checkRecordCombinedFormContractClauseConstructors`
// in TypeResolution.cpp, applying bb2040db's narrow predicate (equal arity,
// same case-sensitive name, resolved type, §22.6 overload modifier, default
// presence) to records as well as classes.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Asserts a §9.3 rejection of `program` with a real source location,
// never a runtime crash.
void expectRecordDuplicateCtorRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.3"));
}

}  // namespace

// ── (1) ISSUE REPRO: record body ctor exactly duplicates the primary
//        — same arity, case-sensitive name, type, modifier, default.
//        MUST REJECT with §9.3 diagnostic ─────────────────────────────────
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           rejects_exact_duplicate_spec_9_3) {
    // `record R(X: int)` + `public constructor(X: int)` with body
    // `this.X := X + 100` — the body ctor is selected (exit 105), which
    // proves the duplicate was admitted instead of rejected.
    expectRecordDuplicateCtorRejected(
        "module M; "
        "record R(X: int) "
        "  public constructor(X: int) "
        "    this.X := X + 100; "
        "  end constructor; "
        "end R; "
        "class P "
        "  public static method Main(): int "
        "    var r := new R(5); "
        "    return r.X; "
        "  end Main; "
        "end P;");
}

// ── (2) SANITY CHECK: case-INSENSITIVE duplicate satisfies §9.8 step 2
//        (which requires binding by case-insensitive name), so this is NOT
//        an exact duplicate and MUST BE ADMITTED ───────────────────────────
//        (§9.3 same-list is case-SENSITIVE per every #4775 reading; this
//        differs at position 0 by case: `X` vs `x`)
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           accepts_case_insensitive_duplicate_satisfies_spec_9_8) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public constructor(x: int) "
        "    this.X := x; "
        "  end constructor; "
        "end R; "
        "class P "
        "  public static method Main(): int "
        "    var r := new R(5); "
        "    return r.X; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ── (3) SANITY CHECK: different parameter name is NOT a duplicate and
//        MUST BE ADMITTED ────────────────────────────────────────────────
//        (differs at position 0 by name: `X` vs `Y`)
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           accepts_different_name_satisfies_spec_9_3) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public constructor(Y: int) "
        "    this.X := Y; "
        "  end constructor; "
        "end R; "
        "class P "
        "  public static method Main(): int "
        "    var r := new R(5); "
        "    return r.X; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ── (4) SANITY CHECK: same arity, same case-sensitive name, DIFFERENT
//        TYPE at one position — this is NOT an exact duplicate of the
//        primary (it fails type resolution against the field type) and
//        MUST BE ADMITTED (a type error may surface separately but it
//        is not a §9.3 duplicate-diagnostic) ─────────────────────────────
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           accepts_same_name_different_type_not_duplicate) {
    // `X: string` against field `X: int` — different type, not a duplicate.
    // Accepts: the body ctor creates a new local `X` that shadows the field.
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public constructor(X: string) "
        "    _ := X; "  // uses the string parameter
        "  end constructor; "
        "end R; "
        "class P "
        "  public static method Main(): int return 0; end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (5) CONTROL: multi-field exact duplicate — same name, same type,
//        same modifier at each position; MUST REJECT with §9.3 ───────────
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           rejects_multi_field_exact_duplicate_spec_9_3) {
    expectRecordDuplicateCtorRejected(
        "module M; "
        "record R(A: int, B: int) "
        "  public constructor(A: int, B: int) "
        "    this.A := A; "
        "    this.B := B; "
        "  end constructor; "
        "end R; "
        "class P "
        "  public static method Main(): int return 0; end Main; "
        "end P;");
}

// ── (6) CONTROL: §9.8 combined-form record with case-insensitive match
//        (the Vec2 pattern) — already accepted, must continue to work.
//        `constructor(x: int, y: int)` against `Vec2(X: int, Y: int)`;
//        §9.8 step 2 REQUIRES this binding. §9.3 status is #4775 Q1/Q2
//        (still open); this row is NOT an exact duplicate (case differs). ─
BEZEL_TEST(record_body_ctor_duplicate_primary_5100,
           accepts_vec2_case_insensitive_combined_form_satisfies_spec_9_8) {
    auto r = runAot(
        "module M; "
        "record Vec2(X: int, Y: int) "
        "  public constructor(x: int, y: int) "
        "    this.X := x; "
        "    this.Y := y; "
        "  end constructor; "
        "end Vec2; "
        "class P "
        "  public static method Main(): int "
        "    var v := new Vec2(3, 4); "
        "    return v.X + v.Y; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
