// fix #843 / §5.1 / §7: a method declared to return a WIDER nullable value type
// that returns a NARROWER numeric value emitted malformed IR — `return 42` from
// a `long?` method `ret`'d a bare i32 against the `{i8, i64}` struct return type
// ("Function return type does not match operand type"). The value must be
// widened to the nullable's inner type before wrapping into the `{i8, T}`
// struct, on BOTH the block-body (ReturnStmt) and the arrow-body (`=> expr`)
// paths. Sibling of the local-decl fix #842.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Block body: `return 42` from a `long?` method (was IR_FAIL).
BEZEL_TEST(nullable_return_widening_843, block_long_from_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): long? return 42; end Mk; "
        "  public static method Main(): int "
        "    var x := C.Mk(); var v: long := x!; "
        "    if v = 42L then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}

// Arrow body: `=> 42` from a `long?` method (separate path, also was IR_FAIL).
BEZEL_TEST(nullable_return_widening_843, arrow_long_from_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): long? => 42; "
        "  public static method Main(): int "
        "    var x := C.Mk(); var v: long := x!; "
        "    if v = 42L then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}

// Block body: int → double? (SIToFP before wrap).
BEZEL_TEST(nullable_return_widening_843, block_double_from_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): double? return 7; end Mk; "
        "  public static method Main(): int "
        "    var x := C.Mk(); var v: double := x!; "
        "    if v = 7.0 then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}

// Arrow body: int → double?.
BEZEL_TEST(nullable_return_widening_843, arrow_double_from_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): double? => 7; "
        "  public static method Main(): int "
        "    var x := C.Mk(); var v: double := x!; "
        "    if v = 7.0 then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}

// Regression: `return null` from a nullable method still produces null.
BEZEL_TEST(nullable_return_widening_843, return_null_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): long? return null; end Mk; "
        "  public static method Main(): int "
        "    var x := C.Mk(); if x = null then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}

// Regression: `int? => 5` (exact inner-type match) still works.
BEZEL_TEST(nullable_return_widening_843, arrow_int_exact_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): int? => 5; "
        "  public static method Main(): int return C.Mk()!; end Main; end C;"),
        5);
}

// Regression: `return 42L` (already i64) still works.
BEZEL_TEST(nullable_return_widening_843, return_long_literal_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Mk(): long? return 42L; end Mk; "
        "  public static method Main(): int "
        "    var x := C.Mk(); var v: long := x!; "
        "    if v = 42L then return 1; end; return 2; "
        "  end Main; end C;"), 1);
}
