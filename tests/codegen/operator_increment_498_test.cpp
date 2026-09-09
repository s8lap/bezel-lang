// Codegen fix #498 — `x++` / `x--` on a user-defined type with
// `static method operator ++` / `operator --` must dispatch to the
// overload and write the result back (per §17.6: `x++ ≡ x := T.operator++(x)`
// and §25 operator-overload resolution).
//
// Regression: before the fix, IncrementStmt unconditionally emitted
// emitCompoundOp("+", cur, 1) on the struct/class value, which is malformed IR
// (sadd.with.overflow on an aggregate / class pointer).
//
// Spec coverage: §17.6 (increment/decrement statement), §25 (operator overloads).
//
// ⚠ RETIRED by fix #3978 (§25.1/§17.6, v0.295). §25.1's not-overloadable
// list, verbatim: "The increment and decrement statements `++` and `--` —
// built-in statements over the operand types §6.6's compound form admits
// (§17.6), not operators of the expression grammar." The dispatch-to-
// overload behavior this file's tests pinned no longer exists —
// `operator ++`/`operator --` is a compile error at its OWN declaration.
// `record_operator_increment_postfix`, `record_operator_decrement_postfix`,
// and `class_operator_increment_local_var` are FLIPPED to REJECT below.
// `record_operator_increment_twice`, `record_operator_increment_in_loop`,
// `record_operator_increment_in_loop_simple`, and
// `record_operator_increment_loop_single_iter` are REMOVED (not flipped):
// each declared the identical `Counter` shape as
// `record_operator_increment_postfix` and differed only in how many times
// the (now uncompilable) `c++` ran afterward — once the declaration itself
// is rejected, all four fail at the exact same diagnostic, so keeping them
// would be four duplicate pins of one rejection rather than four distinct
// regression rows. The `builtin_int_increment_control` /
// `builtin_long_decrement_control` CONTROLs are unaffected and unchanged.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool diagContains498(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}
} // namespace

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "record with operator ++ — c++ increments via overload.
// Equivalent: c := Counter.operator++(c); return c.value; → 1."
BEZEL_TEST(codegen_498, record_operator_increment_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); c++; return c.value; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains498(r, "operator `++` may not be overloaded (§25.1)"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "record with operator -- — c-- decrements via overload.
// Equivalent: c := Counter.operator--(c); return c.value; → 9."
BEZEL_TEST(codegen_498, record_operator_decrement_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator --(c: Counter): Counter => new Counter(c.value - 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(10); c--; return c.value; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains498(r, "operator `--` may not be overloaded (§25.1)"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "class with operator ++ — local class var, operator returns new
// instance. After c++, c.value should be 1."
BEZEL_TEST(codegen_498, class_operator_increment_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public value: int := 0; "
        "  public static method operator ++(b: Box): owned Box "
        "    var r := new Box(); r.value := b.value + 1; return r; "
        "  end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); b.value := 0; b++; return b.value; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains498(r, "operator `++` may not be overloaded (§25.1)"));
}

// CONTROL: built-in int x++ still works — must not regress.
BEZEL_TEST(codegen_498, builtin_int_increment_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 3; x++; return x; "
        "end Main; end C;"), 4);
}

// CONTROL: built-in int x-- still works — must not regress.
BEZEL_TEST(codegen_498, builtin_int_decrement_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 7; x--; return x; "
        "end Main; end C;"), 6);
}

// CONTROL: long x-- — must not regress.
BEZEL_TEST(codegen_498, builtin_long_decrement_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: long := 100L; x--; return int(x); "
        "end Main; end C;"), 99);
}
