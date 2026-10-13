// fix #573 / §19.2 / §7.1 / §7.5: case EXPRESSION with a nullable value-type
// result type (`int?`, `bool?`, nullable record) crashed with
// PHINode::setIncomingValue type-mismatch assertion (SIGABRT) because the arm
// values — a bare scalar from a non-null arm and a ConstantPointerNull from a
// `null` arm — are different LLVM types that cannot share a PHI.  The fix
// applies the same two-path nullable widening that lowerConditional already
// uses (§17 / §7): Path A when the sema result is a nullable VALUE type, and
// Path B when the sema result is a reference type but the arm values are a
// ptr-null vs. non-ptr mismatch.
//
// §19.2: the result type of a case expression is the common type (§20) of all
// non-diverging arm bodies.  §7.1: a nullable value type lowers to a tagged
// {i8 hasValue; T value} struct.  §7.5: a bare T value widens to int? by
// constructing the struct with hasValue=1.

#include "support/aot_support.hpp"   // runAot — AOT oracle (#1767 needs the verifier)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Helper: run a complete program; return the Main exit code.
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Helper: compile+run via the AOT oracle (which runs the IR VERIFIER the ORC
// JIT skips) and return the Main exit code — or -999 if the program was
// rejected (a failed IR verification surfaces here as ran=false). #1767.
int runFullAot(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// run: wrap body in a module + class that returns int? from a helper method
// `Try`, then calls it from Main.  The `innerN` / `expectedN` parameters are
// used to observe the result via a null check + value unwrap → int exit code.
// This helper CANNOT be used here because the test programs must return int
// and observe int? results themselves.  All tests below use runFull directly.

} // namespace

// ── fix #573: case EXPRESSION whose result is int? with a bare-value arm and
// a null arm — the canonical repro from the issue.  Before the fix: SIGABRT
// (PHI type mismatch).  After: compiles and runs correctly. ──────────────────

// §19.2 / §7.1: value arm taken → result is non-null, comparison with null
// is false; method returns 1 (passed).
BEZEL_TEST(codegen_cg28_573, nullable_result_value_arm_nonnull_spec_19_2_7_1) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Try(n: int): int? "
        "    return (case n of "
        "      1 => 100, "
        "      _ => null); "
        "  end Try; "
        "  public static method Main(): int "
        "    var x := C.Try(1); "
        "    return (if x = null then 1 else 0); "
        "  end Main; "
        "end C;"), 0);
}

// §19.2 / §7.1 / §7.2: null arm taken → result is null, comparison with null
// is true; method returns 0 (passed).
BEZEL_TEST(codegen_cg28_573, nullable_result_null_arm_is_null_spec_19_2_7_2) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Try(n: int): int? "
        "    return (case n of "
        "      1 => 100, "
        "      _ => null); "
        "  end Try; "
        "  public static method Main(): int "
        "    var y := C.Try(99); "
        "    return (if y = null then 0 else 1); "
        "  end Main; "
        "end C;"), 0);
}

// §19.2 / §7.5: bare-value arm → the value is unwrappable.  When the result
// is non-null its inner value should equal the arm literal (100).
BEZEL_TEST(codegen_cg28_573, nullable_result_value_correct_spec_19_2_7_5) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Try(n: int): int? "
        "    return (case n of "
        "      1 => 100, "
        "      _ => null); "
        "  end Try; "
        "  public static method Main(): int "
        "    var x := C.Try(1); "
        "    return (if x = null then 0 else (if x <> 100 then 1 else 0)); "
        "  end Main; "
        "end C;"), 0);
}

// §19.2: multiple non-null arms with distinct values plus a null fallback.
// Arm 1 → 10, arm 2 → 20, default → null.
BEZEL_TEST(codegen_cg28_573, nullable_result_multi_value_arms_spec_19_2) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Pick(n: int): int? "
        "    return (case n of "
        "      1 => 10, "
        "      2 => 20, "
        "      _ => null); "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(1); var b := C.Pick(2); var c := C.Pick(9); "
        "    if a = null then return 10; end; "
        "    if b = null then return 20; end; "
        "    if not (c = null) then return 30; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §19.2 / §7.1: all arms produce null.  The result is always null.
BEZEL_TEST(codegen_cg28_573, nullable_result_all_null_arms_spec_19_2) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Always(n: int): int? "
        "    return (case n of "
        "      1 => null, "
        "      _ => null); "
        "  end Always; "
        "  public static method Main(): int "
        "    var x := C.Always(1); "
        "    return (if x = null then 0 else 1); "
        "  end Main; "
        "end C;"), 0);
}

// fix #1767 / §19.2 / §7.1: all arms produce `null`, AOT oracle (with the IR
// verifier the ORC JIT skips). A `null` literal defaults to the nullable
// REFERENCE `Object?` (§7), so a case whose every arm is `null` has sema result
// `Object?` and lowers to a bare `ptr` PHI — but the declared return `int?`
// lowers to the tagged `{i8, i32}` struct. Before the fix the case result was
// `ret`'d as that bare `ptr` (`ret ptr %N` from a `{i8,i32}` fn) — malformed IR
// the JIT ran silently but AOT's verifier REJECTS (so this runAot test fails
// with ran=false pre-fix, passes post-fix). The fix coerces the provably-null
// reference-nullable ptr to the value-nullable `{i8 0, i32 0}` null constant at
// the return, so the `ret` type matches. Always(1)=null → Main returns 0.
BEZEL_TEST(codegen_cg28_573, nullable_result_all_null_arms_aot_1767_spec_19_2) {
    // ran=false (rejection at the verifier) yields -999 here, not 0 — the exact
    // pre-fix failure. Post-fix: compiles, verifies, links, runs → 0.
    BEZEL_EXPECT_EQ(runFullAot(
        "module M; class C "
        "  public static method Always(n: int): int? "
        "    return (case n of "
        "      1 => null, "
        "      _ => null); "
        "  end Always; "
        "  public static method Main(): int "
        "    var x := C.Always(1); "
        "    return (if x = null then 0 else 1); "
        "  end Main; "
        "end C;"), 0);
}

// §19.2 / §7.1 / §7.5: bool? result (same tagged-struct repr as int? but a
// different inner type — i1 inside {i8,i1}).
BEZEL_TEST(codegen_cg28_573, nullable_bool_result_spec_19_2_7_5) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Flag(n: int): bool? "
        "    return (case n of "
        "      1 => true, "
        "      2 => false, "
        "      _ => null); "
        "  end Flag; "
        "  public static method Main(): int "
        "    var a := C.Flag(1); "
        "    var b := C.Flag(3); "
        "    if a = null then return 1; end; "
        "    if not (b = null) then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §19.2 / §7.1: nullable record? result type.  A case expression whose result
// is a record? tagged struct — the arm emits the bare record struct; the null
// arm emits ptr null.  The widening must use the struct's element type to
// build the {i8, record} nullable wrapper.
BEZEL_TEST(codegen_cg28_573, nullable_record_result_spec_19_2_7_1) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public static method Get(n: int): Point? "
        "    return (case n of "
        "      1 => new Point(3, 4), "
        "      _ => null); "
        "  end Get; "
        "  public static method Main(): int "
        "    var p := C.Get(1); "
        "    var q := C.Get(99); "
        "    if p = null then return 1; end; "
        "    if not (q = null) then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// REGRESSION §19.2: a non-nullable case expression (all int arms) must still
// produce the correct int PHI — the widening paths must not fire for it.
BEZEL_TEST(codegen_cg28_573, regression_nonnullable_case_expr_spec_19_2) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Map(n: int): int "
        "    return (case n of "
        "      1 => 10, "
        "      2 => 20, "
        "      _ => 99); "
        "  end Map; "
        "  public static method Main(): int "
        "    return (if C.Map(2) = 20 then 0 else 1); "
        "  end Main; "
        "end C;"), 0);
}

// REGRESSION §19.2 / §7: an existing nullable-VALUE-SCRUTINEE case (int?
// scrutinee, int result — the #447 fix) must still work after the #573 change.
// This exercises the patternMatches nullable-struct path, orthogonal to the
// result-type widening.
BEZEL_TEST(codegen_cg28_573, regression_nullable_scrutinee_int_result_spec_19_7_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of "
        "      42 => 0, "
        "      null => 1, "
        "      _ => 2); "
        "  end Main; "
        "end C;"), 0);
}
