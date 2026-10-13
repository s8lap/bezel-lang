// fix #6005 (§22.6 / §5.1, v0.338): operator overload resolution silently
// picked the first-declared overload where §22.6 makes the call ambiguous.
//
// The operator resolver's per-operand `quality` grader classed an `Object`
// parameter as Inapplicable for ANY non-primitive operand (a class, record,
// or built-in exception). The Animal overload was therefore the only
// survivor for `v + d` with `d: Dog`, and was silently selected. §22.6:
// "`M(a: Animal)` versus `M(o: Object)` on a `Dog` argument is ambiguous --
// neither candidate is exact". §5.1's reference conversion to the Object
// root makes the Object overload applicable (graded Converting), so both
// candidates convert at position 1 and the call is an ambiguous-operator
// compile error. Exact-match control rows (exact beats convert) stay silent.
//
// Repro: #6005 Repro A (user classes) and Repro B (built-in exceptions).

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"           // runAot / runAotChecked
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Returns true when some diagnostic line carrying `sub` starts with the
// exact `:<line>:<col>: ` prefix the Driver prints (Driver.cpp:632). The
// SourceLocation on the JitResult is always {0,0} for AOT rejects, so the
// real position survives only as text (see chain_position_field_read_3829).
bool diagnosticAtExactPosition(const bezel::test::JitResult& r,
                               const char* sub, int line, int col) {
    const std::string needle = ":" + std::to_string(line) + ":" +
                               std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(sub) == std::string::npos) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Repro A — user classes: Animal and Object overloads on a Dog operand.
// Expected (§22.6): ambiguous operator `+`. Pre-fix: prints 1.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t1_animal_vs_object_user_class_ambiguous_rejected_spec_22_6) {
    auto r = runAot(
        "module M; class Animal end Animal; class Dog: Animal end Dog; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Animal): V => "
        "new V(a.N + 1); "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var d := new Dog(); var v := new V(0); "
        "  Console.WriteLine((v + d).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous operator `+`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, Animal)`, `operator +(V, Object)`"));
    // The binary expression's span begins at `v` (1-based column 352 of this
    // single source line), MEASURED with `bezelc -o` on the same source.
    BEZEL_EXPECT_TRUE(diagnosticAtExactPosition(
        r, "ambiguous operator `+`", 1, 352));
}

// ---------------------------------------------------------------------------
// Repro B — built-in exceptions: Exception and Object overloads on an
// ArgumentException operand. Expected (§22.6): ambiguous operator `+`.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t2_exception_vs_object_builtin_ambiguous_rejected_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Exception): V => "
        "new V(a.N + 1); "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var d := new ArgumentException(\"x\"); var v := new V(0); "
        "  Console.WriteLine((v + d).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous operator `+`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, Exception)`, `operator +(V, Object)`"));
}

// ---------------------------------------------------------------------------
// Accept row — a lone `Object` overload is applicable to a Dog operand by the
// §5.1 reference conversion. Pre-fix it was Inapplicable and the operator
// was rejected as undefined; post-fix it resolves and prints 9 (§5.1 / §25).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t3_lone_object_overload_applicable_to_class_operand_spec_5_1) {
    auto r = runAot(
        "module M; class Animal end Animal; class Dog: Animal end Dog; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var d := new Dog(); var v := new V(0); "
        "  Console.WriteLine((v + d).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ---------------------------------------------------------------------------
// Gap guard #6059 (enum operand). Value-kind operand (enum) against a lone
// `Object` operator parameter. This row is rejected today only because operator
// lowering does not box value kinds to Object; it flips to accept+run when #6059
// lands (§5.1). Until then the overload stays Inapplicable and the call is the
// clean undefined-operator compile error at 1:236 (guards the malformed-IR path,
// which must not return).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t5_enum_operand_gap_guard_6059_rejected_clean_spec_5_1) {
    auto r = runAot(
        "module M; enum Color Red, Green; end Color; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var v := new V(0); "
        "  Console.WriteLine((v + Color.Red).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `V`"));
    BEZEL_EXPECT_TRUE(diagnosticAtExactPosition(
        r, "operator `+` is not defined for type `V`", 1, 236));
}

// ---------------------------------------------------------------------------
// Gap guard #6059 (record operand). Value-kind operand (record) against a lone
// `Object` operator parameter. This row is rejected today only because operator
// lowering does not box value kinds to Object; it flips to accept+run when #6059
// lands (§5.1). Until then the overload stays Inapplicable and the call is the
// clean undefined-operator compile error at 1:226 (guards the malformed-IR path,
// which must not return).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t6_record_operand_gap_guard_6059_rejected_clean_spec_5_1) {
    auto r = runAot(
        "module M; record R(X: int) end R; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var v := new V(0); "
        "  Console.WriteLine((v + new R(1)).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `V`"));
    BEZEL_EXPECT_TRUE(diagnosticAtExactPosition(
        r, "operator `+` is not defined for type `V`", 1, 226));
}

// ---------------------------------------------------------------------------
// Gap guard #6059 (choice operand). Value-kind operand (choice) against a lone
// `Object` operator parameter. This row is rejected today only because operator
// lowering does not box value kinds to Object; it flips to accept+run when #6059
// lands (§5.1). Until then the overload stays Inapplicable and the call is the
// clean undefined-operator compile error at 1:240 (guards the malformed-IR path,
// which must not return).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t7_choice_operand_gap_guard_6059_rejected_clean_spec_5_1) {
    auto r = runAot(
        "module M; choice Sh Circle(r: int), Sq; end Sh; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var v := new V(0); "
        "  Console.WriteLine((v + Sh.Sq).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `V`"));
    BEZEL_EXPECT_TRUE(diagnosticAtExactPosition(
        r, "operator `+` is not defined for type `V`", 1, 240));
}

// ---------------------------------------------------------------------------
// Control — exact beats convert: a `Dog` overload is exact for a Dog operand
// and wins over the Object overload (§22.6). Must stay silent: prints 1.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_object_tie_6005,
           t4_exact_overload_beats_object_overload_spec_22_6) {
    auto r = runAot(
        "module M; class Animal end Animal; class Dog: Animal end Dog; "
        "record V(N: int) "
        "  public static pure method operator +(a: V, e: Dog): V => "
        "new V(a.N + 1); "
        "  public static pure method operator +(a: V, e: Object): V => "
        "new V(a.N + 9); "
        "end V; "
        "class P public static method Main(): int "
        "  using var d := new Dog(); var v := new V(0); "
        "  Console.WriteLine((v + d).N); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}
