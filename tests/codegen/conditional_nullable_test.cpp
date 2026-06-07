// Codegen — conditional expression `(V if C else null)` / `(null if C else V)`
// whose result type is a NULLABLE VALUE TYPE crashes with a PHI type mismatch
// (§17 conditional expression; §7 nullable value types).
//
// When the conditional result type (from sema) is a nullable value type (int?,
// bool?, double?, enum?, nullable record) the two branch values must both be
// lifted to the tagged {i8 hasValue; T value} struct BEFORE the PHI node is
// created.  Before this fix the non-null branch produced a bare T (e.g. i32)
// while the null branch produced the struct, so the PHI merged two different
// LLVM types and the verifier/assertion `getType() == V->getType()` in
// PHINode::setIncomingValue aborted the compiler.
//
// Fix location: Emitter::lowerConditional (src/codegen/LlvmBackend_Exprs.cpp).
// Widening helper reused: wrapNullableValue (wraps bare T → {hasValue=1, T}),
// nullableValueNull (produces all-zero {hasValue=0, T=0}).
//
// These tests verify that the crash is gone AND that the run-time values are
// correct.  The null/non-null state is observed via the existing `= null`
// comparison (§8.4) and the `!` force-unwrap (§7.8); the `??` coalesce (§17.1)
// covers the null branch.  Control tests confirm non-nullable conditionals and
// nullable reference conditionals are not disturbed.
//
// Spec derivation: §17 (conditional expression), §7.1 (nullable types),
// §7.2 (null literal), §7.8 (force-unwrap !), §8.4 (null-safe =/<>),
// §17.1 (?? coalesce).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a standard class shell.
int runMain(const std::string& body) {
    return run(
        "module M; class C public static method Main(): int " +
        body + " end Main; end C;");
}

// Wrap a Main body that uses an enum Color {Red=0,Green=1,Blue=2}.
int runEnum(const std::string& body) {
    return run(
        "module M; enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int " +
        body + " end Main; end C;");
}

// Wrap a Main body that uses a Box record (V: int).
int runBox(const std::string& body) {
    return run(
        "module M; record Box(V: int); "
        "class C public static method Main(): int " +
        body + " end Main; end C;");
}

// A helper class T with a V:int field (for the nullable-reference control test).
int runRef(const std::string& body) {
    return run(
        "module M; class T public V: int := 0; end T; "
        "class C public static method Main(): int " +
        body + " end Main; end C;");
}

} // namespace

// ── (a) int? — true branch (non-null), §17/§7 ─────────────────────────────

// `(42 if true else null)` yields int? with hasValue=1 and value=42.
// Before fix: LLVM PHI assertion abort (i32 vs {i8,i32} mismatch).
BEZEL_TEST(codegen_conditional_nullable, int_nullable_true_branch_has_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (42 if true else null); "
        "if r = null then return -1; end; "
        "return r!;"), 42);
}

// The non-null value can be observed via `??` coalesce (§17.1): since r is
// non-null, `r ?? -1` returns r's value (42).
BEZEL_TEST(codegen_conditional_nullable, int_nullable_true_branch_coalesce_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (42 if true else null); "
        "return r ?? -1;"), 42);
}

// ── (b) int? — false branch (null), §17/§7 ────────────────────────────────

// `(42 if false else null)` yields int? with hasValue=0 (null).
// `?? -1` returns the right operand (-1) because r is null.
BEZEL_TEST(codegen_conditional_nullable, int_nullable_false_branch_is_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (42 if false else null); "
        "return r ?? -1;"), -1);
}

// Confirm with `= null` (§8.4): a null int? is `= null`.
BEZEL_TEST(codegen_conditional_nullable, int_nullable_false_branch_equals_null_spec_8_4) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (42 if false else null); "
        "return int(r = null);"), 1);
}

// ── (c) reversed `(null if C else V)`, §17/§7 ─────────────────────────────

// `(null if false else 7)` — null branch first, non-null branch second.
// Condition false → else branch (7) taken; result int? with hasValue=1.
BEZEL_TEST(codegen_conditional_nullable, int_nullable_null_first_else_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (null if false else 7); "
        "return r ?? -1;"), 7);
}

// `(null if true else 7)` — null branch taken; result is null.
BEZEL_TEST(codegen_conditional_nullable, int_nullable_null_first_true_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: int? := (null if true else 7); "
        "return int(r = null);"), 1);
}

// ── variable condition (flag) for both orders ──────────────────────────────

// Condition from a variable: both true/false paths exercise real PHI merging.
BEZEL_TEST(codegen_conditional_nullable, int_nullable_var_cond_true_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var flag := true; "
        "var r: int? := (42 if flag else null); "
        "return r ?? -1;"), 42);
}

BEZEL_TEST(codegen_conditional_nullable, int_nullable_var_cond_false_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var flag := false; "
        "var r: int? := (42 if flag else null); "
        "return r ?? -1;"), -1);
}

// ── (d) bool? variant, §17/§7 ─────────────────────────────────────────────

// `(true if true else null)` → bool? holding true.  Unwrap with !; int(true)=1.
BEZEL_TEST(codegen_conditional_nullable, bool_nullable_true_branch_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: bool? := (true if true else null); "
        "if r = null then return -1; end; "
        "return int(r!);"), 1);
}

// `(true if false else null)` → bool? is null.
BEZEL_TEST(codegen_conditional_nullable, bool_nullable_false_branch_is_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: bool? := (true if false else null); "
        "return int(r = null);"), 1);
}

// ── (d) double? variant, §17/§7 ───────────────────────────────────────────

// `(3.5 if true else null)` → double? holding 3.5.  Read back via !.
// We convert to int (truncate) for observable exit code: int(3.5) = 3.
BEZEL_TEST(codegen_conditional_nullable, double_nullable_true_branch_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: double? := (3.5 if true else null); "
        "if r = null then return -1; end; "
        "return int(r!);"), 3);
}

// `(3.5 if false else null)` → double? is null.
BEZEL_TEST(codegen_conditional_nullable, double_nullable_false_branch_is_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runMain(
        "var r: double? := (3.5 if false else null); "
        "return int(r = null);"), 1);
}

// ── (d) enum? variant, §17/§7/§10 ─────────────────────────────────────────

// `(Color.Green if true else null)` → Color? holding Green (value 1).
// int(Color.Green) = 1.
BEZEL_TEST(codegen_conditional_nullable, enum_nullable_true_branch_spec_17_7_10) {
    BEZEL_EXPECT_EQ(runEnum(
        "var r: Color? := (Color.Green if true else null); "
        "if r = null then return -1; end; "
        "return int(r!);"), 1);
}

// `(Color.Green if false else null)` → Color? is null. Must not equal Red (0).
BEZEL_TEST(codegen_conditional_nullable, enum_nullable_false_branch_is_null_spec_17_7_10) {
    BEZEL_EXPECT_EQ(runEnum(
        "var r: Color? := (Color.Green if false else null); "
        "return int(r = null);"), 1);
}

// ── (e) nullable record variant, §17/§7.2/§9 ──────────────────────────────

// `(new Box(9) if true else null)` → Box? holding the record (V=9).
// Narrow via `<> null` then read V.
BEZEL_TEST(codegen_conditional_nullable, record_nullable_true_branch_spec_17_7) {
    BEZEL_EXPECT_EQ(runBox(
        "var r: Box? := (new Box(9) if true else null); "
        "if r <> null then return r!.V; end; "
        "return -1;"), 9);
}

// `(new Box(9) if false else null)` → Box? is null.
BEZEL_TEST(codegen_conditional_nullable, record_nullable_false_branch_is_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runBox(
        "var r: Box? := (new Box(9) if false else null); "
        "return int(r = null);"), 1);
}

// Reversed: `(null if false else new Box(5))` — value branch is the else arm.
BEZEL_TEST(codegen_conditional_nullable, record_nullable_null_first_else_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runBox(
        "var r: Box? := (null if false else new Box(5)); "
        "if r <> null then return r!.V; end; "
        "return -1;"), 5);
}

// ── (f) CONTROL: `(1 if C else 2)` non-nullable int — unchanged, §17 ──────

// A plain non-nullable conditional must still work correctly after the fix.
BEZEL_TEST(codegen_conditional_nullable, control_non_nullable_int_true_spec_17) {
    BEZEL_EXPECT_EQ(runMain(
        "return (1 if true else 2);"), 1);
}

BEZEL_TEST(codegen_conditional_nullable, control_non_nullable_int_false_spec_17) {
    BEZEL_EXPECT_EQ(runMain(
        "return (1 if false else 2);"), 2);
}

// ── (g) CONTROL: nullable REFERENCE conditional — unchanged, §17/§7 ───────

// For a nullable reference type (class?) the null is already a ptr null and both
// branches share the pointer type; widening to a struct must NOT happen. Confirm
// the behaviour is preserved.
BEZEL_TEST(codegen_conditional_nullable, control_nullable_ref_non_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runRef(
        "var a := new T(); a.V := 11; "
        "var r: T? := (a if true else null); "
        "if r = null then return -1; end; "
        "return r!.V;"), 11);
}

BEZEL_TEST(codegen_conditional_nullable, control_nullable_ref_null_spec_17_7) {
    BEZEL_EXPECT_EQ(runRef(
        "var a := new T(); a.V := 11; "
        "var r: T? := (a if false else null); "
        "return int(r = null);"), 1);
}
