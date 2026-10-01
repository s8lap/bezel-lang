// Regression tests for issue #4958 — `x as T` on a CONCRETE non-nullable
// RECORD operand lowered into an IR verifier abort ("Invalid InsertValueInst
// operands" at `%as.wrap`).
//
// REGRESSION PIN (§17.4, v0.307). Issue #4958: `(p as Pt)!` where `p` is a
// concrete record value `Pt` failed LLVM IR verification with "Invalid
// InsertValueInst operands" at the `as.wrap` insertValue. The probe is the
// value-type cast on a record — `p as Pt` — followed by the `!` unwrap.
//
// THE DEFECT. lowerTypeTest's `concreteValueTypeOperand` arm (added by
// fix #3120 to handle non-pointer VALUE-type operands) wrapped the operand
// into a nullable struct via `nullableValueStructTy(rt)` where `rt` is the
// testType's NON-NULLABLE TypeRef (`Pt`, not `Pt?`). nullableValueStructTy
// gates every dispatch on `rt->nullable` — record → `{i8, T}`; primitive
// → `{i8, primitive}`; etc. — so passing non-nullable `Pt` made it return
// the WRONG struct: the i32 primitive layout `{i8, i32}`. The insertValue
// at `as.wrap` then refused the `%record.Pt` operand ("Invalid InsertValueInst
// operands") and `verifyModule` aborted bezelc with a codegen error.
//
// THE FIX. Build a nullable copy of `rt` for the type-shape queries — `as T`
// produces `T?` per spec §17.4, and that promotion is the TypeTestExpr's
// own exprType, NOT `te.testType`'s. The operand value `obj` already carries
// its own (non-nullable) type so its insertValue lands cleanly.
//
// ANTI-REGRESSION COVERAGE. Three shapes (each a separate test so the
// failure surface is the right one):
//   (a) The exact probe: a record local, `(p as Pt)!` returning a value.
//       Pre-fix: bezelc IR verify failure. Post-fix: prints "3" and exits.
//   (b) `as Pt` as the argument to a constructor. The same wrap path; pins
//       that the wrap also works when the result feeds a callee.
//   (c) Cross-type mismatch (p is Pt, target is Q): the `nullableValueNull`
//       arm must also use the nullable struct shape (the sibling bug at
//       the same call site).

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

// (a) The issue's headline shape: `(p as Pt)!` returns q.X.
// Pre-fix: bezelc IR verify failure with "Invalid InsertValueInst operands"
// at `%as.wrap`. Post-fix: prints "3" (the X field) and exits 0.
BEZEL_TEST(as_cast_value_typed_record_4958,
           record_local_as_same_record_bang_returns_field) {
    auto r = runAotChecked(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class H "
        "  public static method ReadIt(p: Pt): int "
        "    var q := (p as Pt)!; "
        "    return q.X; "
        "  end ReadIt; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt(3, 4); "
        "    Console.WriteLine(H.ReadIt(p)); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// (b) `as Pt` as an argument to a constructor: `(p as Pt)!.X` is the ctor's
// int argument. Pins that the wrap path also works when the result feeds a
// callee (the wrapped value flows through to a non-nullable parameter, so
// the callee receives the inner i32).
BEZEL_TEST(as_cast_value_typed_record_4958,
           record_local_as_same_record_passes_value_to_ctor) {
    auto r = runAotChecked(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class W "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method V(): int => _v; "
        "  private _v: int; "
        "end W; "
        "class H "
        "  public static method UseIt(p: Pt): int "
        "    var w := new W((p as Pt)!.X); "
        "    return w.V(); "
        "  end UseIt; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt(3, 4); "
        "    Console.WriteLine(H.UseIt(p)); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// (c) §12.7 (v0.310/#5157) — flip: `p as Q` where p is Pt was admit
// (produces null, ?? returns new Q(0).A = 0), now reject (different concrete
// record types). Assert the diagnostic before the ran-check — otherwise a
// program that neither compiles nor runs vacuously passes the test.
BEZEL_TEST(as_cast_value_typed_record_4958,
           record_as_other_record_rejected_spec_12_7) {
    auto r = runAotChecked(
        "module M; "
        "record Pt(X: int, Y: int); "
        "record Q(A: int); "
        "class H "
        "  public static method MissIt(p: Pt): int "
        "    var rr := (p as Q); "
        "    return (rr ?? new Q(0)).A; "
        "  end MissIt; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt(3, 4); "
        "    Console.WriteLine(H.MissIt(p)); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // per §12.7 (v0.313): the specific other-value-type reject, at a real
    // location, not just "something failed" (#3892).
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "a different value type is a compile error"));
}

}  // namespace
}  // namespace bezel::codegen
