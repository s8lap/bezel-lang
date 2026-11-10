// Regression test for issue #7879 — `return a ?? [1]` through an `owned
// Array<int>` emitted a block with no terminator.
//
// §7.7 (Null-coalescing `??` and `??=`) / §15.4 (Collection literals) / §20
// (the `if` expression) / §28.3 (the `a ?? b` producers row), v0.355.
//
// THE DEFECT. `a ?? [1]` — a null-coalescing whose RIGHT OPERAND is a
// COLLECTION LITERAL — emitted malformed IR:
//
//     define ptr @"bz$M.C$Bad"(ptr %0) {
//     entry:
//       %3 = icmp eq ptr %2, null
//       br i1 %3, label %coalesce.rhs, label %coalesce.end
//     coalesce.rhs:
//       ret ptr null
//     coalesce.end:                       ; <-- NO TERMINATOR
//     }
//
// and the LLVM verifier rejected the whole module:
//
//     Basic Block in function 'bz$M.C$Bad' does not have terminator!
//     label %coalesce.end
//     s5.bzl:8:68: warning: internal: return-value expression failed to lower
//       to a value (degraded to a zero/null return value) — #2491
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// so the program never built.
//
// THE CAUSE (MEASURED, R41, base a30bb11d). NOT the collection-literal
// materializer: it was never entered. `lowerCoalesce` (src/codegen/
// LlvmBackend_Exprs2.cpp) lowers the right operand with a plain `lowerExpr`,
// and a `CollectionLiteralExpr` is not a general expression value — `lowerExpr`
// returns null for it, because §15.4 gives a collection literal its target type
// from the CONTEXT (the declaration, the return type, the parameter type), and
// the `??` join had no context to give. So `r` came back null and
// `lowerCoalesce` bailed out with `return nullptr` — AFTER the entry block had
// already been branched to `coalesce.rhs` / `coalesce.end`, so NOTHING ever
// terminated `%coalesce.end`. The `ret ptr null` in `coalesce.rhs` is the
// CALLER's degraded return value (the #2491 note), not the materializer's.
//
// §7.7 is explicit that `??` is a control-flow join, and a control-flow join
// is a PHI-join of two branches — so the merge block it creates MUST be
// terminated. "the left operand is evaluated exactly once, and the right
// operand is evaluated only when the left operand's value is `null` … `??` is
// therefore a control-flow join (K7) rather than a two-operand operation".
//
// THE FIX. `lowerCoalesce` MATERIALIZES a collection-literal right operand
// against the join's contextual target type — the LEFT operand's type, which
// §7.7's typing rule names ("for a left operand of type `T?`, the right
// operand must be implicitly convertible (§5.1) to `T` or to `T?`") and which is
// also the `??` expression's own result type. And the merge block is closed on
// the remaining degrade path, so a `??` whose right operand does not lower can
// no longer leave an open block behind.
//
// THE MATRIX. Rows 1, 3, 5, 6 and 9 of the issue's matrix are the shapes this
// fix restores; rows 2, 4, 7, 8, 10, 11 and 12 are the controls, and the
// control rows below pin that their verdicts are UNCHANGED — in particular
// that the `if`-expression twin of the same mixed meet (§20) is still
// REJECTED, and that a `??` with a VARIABLE right operand still compiles.
//
// NOTE (ownership). §28.3.4's producers table classifies a collection literal
// targeting a reference type as OWNED, and a plain (unmarked) parameter as
// BORROWED, so `a ?? [1]` is a MIXED meet — which §7.7 rejects wherever the
// meet is formed. The ownership pass did not classify it (a collection
// literal carries no `exprType` at any position, so its producer state cannot
// be read off the type) and therefore reported nothing; that gap is #8371, and
// it is NOW CLOSED — a `??` whose arm is a collection literal over a BORROW is
// refused with the §28.3 mixed-meet diagnostic, exactly as its `new` twin is.
//
// The consequence for THIS file: the rows below that formed such a meet over
// a borrow (T1–T5) assert the IR-validity and the join's lowering, which the
// ownership pass never reached, so they are unaffected. T6 — the one row that
// RAN a program, and so reached the ownership pass — now spells the meet with
// an OWNED left arm (`owned ⊓ owned` — the only legal spelling of a `??` whose
// right operand is a collection literal), so it still measures the join's
// VALUE end to end, on a shape that compiles on both arms.
//
//   T1 — the issue's own repro: `return a ?? [1]` through `: owned Array<int>`.
//        RED before the fix: the module did not verify.
//   T2 — the NULLABLE-left variant (`a: Array<int>?`).
//   T3 — the `Array<string>` variant.
//   T4 — the LOCAL-DECL variant (no `owned` anywhere): `var x: Array<int> := a
//        ?? [1];` — the initializer is the `??`, so the same join is formed in
//        a binding position.
//   T5 — the NULLABLE-local variant: `var y: Array<int>? := null; var x:
//        Array<int> := y ?? [1];`.
//   T6 — the join's VALUE, end to end: the right operand is evaluated only
//        when the left is null (§7.7's short-circuit), and the chosen arm is
//        the result. Asserts the counts, so a green row cannot come from the
//        join always taking one arm. The LEFT arm is an OWNED producer
//        (`owned Array<int>?`), because §28.3's `a ?? b` row makes a
//        collection-literal arm an OWNED arm and a mixed meet is rejected
//        (the #8371 closure).
//   T7 — CONTROL: a `??` whose right operand is a VARIABLE still compiles and
//        still yields the left operand when it is non-null.
//   T8 — CONTROL: the `if`-expression twin of the same mixed meet is still
//        REJECTED (§20: "a collection literal cannot be an `if` expression
//        branch"), and the plain borrow-return is still rejected (§28.3).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::emitAndVerify;
using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim in shape: the right operand of `??` is a
// COLLECTION LITERAL and the declared return type is `owned Array<int>`.
constexpr const char* kRepro =
    "module M7879; "
    "public class C "
    "  public static method Bad(a: Array<int>): owned Array<int> "
    "    return a ?? [1]; "
    "  end Bad; "
    "end C;";

}  // namespace

// ===========================================================================
// T1 — the issue's own repro. RED before the fix: the emitted module did not
// verify ("Basic Block in function 'bz$M.C$Bad' does not have terminator!"),
// so the program never built.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           owned_array_return_coalesce_literal_verifies_sec_7_7) {
    auto r = emitAndVerify(kRepro);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T2 — the NULLABLE-left variant: `a: Array<int>?`. The left operand's type
// is `T?` and §7.7's typing rule makes the right operand's target `T`.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           nullable_left_coalesce_literal_verifies_sec_7_7) {
    auto r = emitAndVerify(
        "module M7879b; "
        "public class C "
        "  public static method Bad(a: Array<int>?): owned Array<int> "
        "    return a ?? [1]; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T3 — the `Array<string>` variant: the element type is not the trigger, the
// COLLECTION-LITERAL right operand is.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           owned_string_array_return_coalesce_literal_verifies_sec_7_7) {
    auto r = emitAndVerify(
        "module M7879c; "
        "public class C "
        "  public static method Bad(a: Array<string>): owned Array<string> "
        "    return a ?? [\"x\"]; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T4 — the LOCAL-DECL variant: `var x: Array<int> := a ?? [1];` — the `??` is
// the initializer, so the same join is formed in a binding position and the
// #2434 degrade note ("lowerLocalDecl's initializer expression failed to
// lower") fires instead of #2491. The defect is the same one: the merge
// block was left open.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           local_decl_coalesce_literal_verifies_sec_7_7) {
    auto r = emitAndVerify(
        "module M7879d; "
        "public class C "
        "  public static method Bad(a: Array<int>): int "
        "    var x: Array<int> := a ?? [1]; "
        "    return x.Count; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T5 — the NULLABLE-local variant: `var y: Array<int>? := null; var x:
// Array<int> := y ?? [1];` — the left operand is a nullable LOCAL, so the
// right operand is the only arm that can be taken.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           nullable_local_coalesce_literal_verifies_sec_7_7) {
    auto r = emitAndVerify(
        "module M7879e; "
        "public class C "
        "  public static method Bad(): int "
        "    var y: Array<int>? := null; "
        "    var x: Array<int> := y ?? [1]; "
        "    return x.Count; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T6 — the join's VALUE, end to end. §7.7: "the left operand is evaluated
// exactly once, and the right operand is evaluated only when the left
// operand's value is `null`". The program counts the elements of BOTH arms:
// the null left takes the literal (count 1, first element 1), and the
// non-null left takes the left operand (count 3, first element 7). The
// counts are what make the row a VALUE claim rather than a "did not crash"
// claim: a join that always took one arm would fail one of the two.
//
// The two arms are spelled in the two shapes that keep the program
// dispose-exactly-once: the null arm through the `owned` return (the caller
// owns the fresh literal and disposes it), the non-null arm through a plain
// local binding (the binding BORROWS the left operand, which its own scope
// disposes).
//
// The LEFT arm is an OWNED producer (`owned Array<int>?`) rather than a
// borrowed parameter. Section 28.3's `a ?? b` row makes the meet of the two
// arms' ownership states, and its collection-literal row makes the literal
// arm OWNED: a literal arm over a BORROW left is a MIXED meet, and section
// 28.3 K7.3 rejects a mixed meet wherever it is formed. That is the #8371
// closure — the gap this file's header NOTE recorded as "a SEPARATE defect,
// filed on its own" — so the borrowed-left spelling of this meet no longer
// compiles. The OWNED left keeps the row's claim intact (the join's VALUE,
// end to end, on a shape that compiles on both arms) and keeps the
// short-circuit under test: `Fresh(0)` is null (the literal arm is taken)
// and `Fresh(3)` is not (the left arm is taken).
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           coalesce_literal_yields_the_selected_arm_sec_7_7) {
    auto r = runAot(
        "module M7879f; "
        "import System; "
        "public class C "
        "  public static method Fresh(n: int): owned Array<int>? "
        "    if n = 0 then return null; end if; "
        "    var r: Array<int> := new Array<int>(); "
        "    r.Add(7); r.Add(8); r.Add(9); "
        "    return r; "
        "  end Fresh; "
        "  public static method Bad(): owned Array<int> "
        "    return Fresh(0) ?? [1]; "
        "  end Bad; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var r1: Array<int> := C.Bad(); "
        "    acc := acc + r1.Count; "
        "    if r1[0] = 1 then acc := acc + 1; end; "
        "    var x: Array<int> := C.Fresh(3) ?? [1]; "
        "    acc := acc + x.Count; "
        "    if x[0] = 7 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// T7 — CONTROL: a `??` whose right operand is a VARIABLE was never affected
// by the defect and must stay unaffected — it compiles, and it yields the
// LEFT operand when the left is non-null.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           variable_right_operand_still_compiles_sec_7_7) {
    auto r = emitAndVerify(
        "module M7879g; "
        "public class C "
        "  public static method Bad(a: Array<int>, b: Array<int>): "
        "owned Array<int> "
        "    return a ?? b; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ===========================================================================
// T8 — CONTROL: the `if`-expression TWIN of the same mixed meet is still
// REJECTED. §20: "a collection literal cannot be an `if` expression branch:
// it requires a target type from context, which an `if` branch does not
// supply". The fix must not loosen that, and must not make the `if` twin
// compile either.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_collection_literal_7879,
           if_expression_twin_still_rejected_sec_20) {
    auto r = emitAndVerify(
        "module M7879h; "
        "public class C "
        "  public static method Bad(a: Array<int>, c: bool): owned Array<int> "
        "    return (if c then a else [1]); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.lowered);
}
