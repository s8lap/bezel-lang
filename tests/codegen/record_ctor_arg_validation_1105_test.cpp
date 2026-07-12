// Regression tests for #1105 (§9.1): a record's positional fields ARE its
// primary constructor (one argument per field, in declaration order, no
// chaining, no per-field defaults). `checkNewExprArgs` returned early for any
// `hasPositionalFields` type, so RECORD construction got NO argument checking:
// too-few / too-many / wrong-typed args were silently accepted and miscompiled
// (`new Pt(1)`→Y=0; `new Pt("a","b")`→garbage; `new Pt(3.14,..)`→int
// bit-pattern; some wrong-typed forms crashed IR verification). Sema now
// validates a non-generic positional record's `new` args (count + per-field
// assignability) and rejects mismatches with a §9.1 diagnostic. Found by
// adversarial discovery sweep #9.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
}
}  // namespace

// #1105: too FEW arguments → rejected (was: accepted, missing field zeroed).
BEZEL_TEST(record_ctor_arg_validation_1105, too_few_args_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(1); return p.X; "
        "end Main; end C;"));
}

// #1105: too MANY arguments → rejected.
BEZEL_TEST(record_ctor_arg_validation_1105, too_many_args_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2, 3); return p.X; "
        "end Main; end C;"));
}

// #1105: ZERO arguments → rejected.
BEZEL_TEST(record_ctor_arg_validation_1105, zero_args_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(); return 0; "
        "end Main; end C;"));
}

// #1105: wrong-typed args (string where int) → rejected (was: miscompiled).
BEZEL_TEST(record_ctor_arg_validation_1105, wrong_type_string_for_int_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(\"a\", \"b\"); return 0; "
        "end Main; end C;"));
}

// #1105: double literal for an int field → rejected (was: int bit-pattern
// garbage, rc=31 for 3.14).
BEZEL_TEST(record_ctor_arg_validation_1105, double_for_int_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3.14, 2.71); return p.X; "
        "end Main; end C;"));
}

// Regression guard: a correctly-constructed record is unchanged.
BEZEL_TEST(record_ctor_arg_validation_1105, correct_construction_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); return p.X * p.Y; "    // 12
        "end Main; end C;"), 12);
}

// Regression guard: a valid widening conversion (int→long field) is accepted.
BEZEL_TEST(record_ctor_arg_validation_1105, widening_arg_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(X: long, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(5, 7); return int(p.X) + p.Y; "  // 12
        "end Main; end C;"), 12);
}

// Regression guard: string + nested-record fields construct correctly.
BEZEL_TEST(record_ctor_arg_validation_1105, string_and_nested_record_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(A: int); record Outer(I: Inner, B: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(5), 7); return o.I.A + o.B; "  // 12
        "end Main; end C;"), 12);
}

// Regression guard: a GENERIC record is unaffected (its per-instantiation
// field-type substitution stays deferred to the #428 family; construction with
// the right shape still works).
BEZEL_TEST(record_ctor_arg_validation_1105, generic_record_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "class C public static method Main(): int "
        "  var b := new Box<int>(7); return b.V; "       // 7
        "end Main; end C;"), 7);
}
