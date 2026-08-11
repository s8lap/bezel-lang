// Spec-derived regression test for issue #2883.
//
// §6.6 (Compound assignments) / §17.6 (The increment and decrement operators) /
// §6.4 (Decimal arithmetic) / §5.1 (Implicit conversions) / §23.3 (Block-form
// properties), v0.240.
//
// THE DEFECT. A `decimal`-typed PROPERTY or INDEXER compound assignment /
// increment aborted the compile with an IR-verifier error — a legal program
// producing an ICE, not a diagnostic. Measured on `2aed89a`:
//
//     d.P  += 1m;   ->  Intrinsic has incorrect return type!
//                       ptr @llvm.sadd.with.overflow.s_bz.decimals
//     d[0] += 1m;   ->  (same, twice — `+=` and `++`)
//     d[0]++;
//     i.P  += 3m;   ->  (same, through an interface-typed receiver)
//     d.P++;        ->  Call parameter type does not match function signature!
//                       i33 1 … @llvm.sadd.with.overflow.s_bz.decimals
//
// THE CAUSE. `decimal` is a STRUCT-typed primitive (`%bz.decimal`, §5.1/§6.4),
// but `Emitter::emitCompoundOp` had arms only for `char` (#1961), string
// concatenation (#857), floating point, and the integer family — no decimal
// arm. A `%bz.decimal` left operand therefore fell through to the integer tail
// and was handed to `llvm.sadd.with.overflow`, which the IR verifier rejects.
// The LOCAL statement paths (`d += x`, `d++` on a decimal local or field) were
// already correct: they intercept with an explicit `elemTy == decimalStructType()`
// branch that calls `emitDecimalArith`. Four callers reached the missing arm —
// `lowerPropertyCompound`, `lowerIndexerCompoundCore`, the #481 virtual/
// interface property arm of `lowerStmt`, and `lowerImportedPropertyCompoundCore`
// (which #2862 had to open-code its own caller-side decimal check to dodge).
//
// The `d.P++` spelling failed one step earlier: the `computedSetterOf` branch
// of `lowerStmt`'s `IncrementStmt` arm synthesized §17.6's implicit `1` with
// `llvm::ConstantInt::get(cur->getType(), 1)`, which on a struct type does not
// build an integer constant of that type at all (it produced a nonsense
// `i33 1`).
//
// THE FIX gives `emitCompoundOp` ITSELF the decimal arm — routing a
// `%bz.decimal` left operand to `emitDecimalArith`, which carries §6.4's
// "raises `OverflowException` on overflow and `DivideByZeroException` on
// division by zero" — and DELETES #2862's now-redundant caller-side copy. The
// `++` site synthesizes its `1` via `emitDecimalOne()`, gated on the LOWERED
// type exactly as its two sibling sites already gate it.
//
// Every case below asserts a CORRECT VALUE, not merely that the program
// compiles. Decimal observables travel on stdout via `runAotOut` (a decimal
// does not fit an 8-bit exit status); the exception cases use the exit code.
//
// NOT covered here, and NOT fixed by this change: a NULLABLE-value-typed
// property or indexer compound (`b.NP += 3` where `NP: int?`) is a separate
// live ICE of the same shape — the §7.11 lifting exists only on the local/field
// path. Reproduced for `int?` (property), `long?` (indexer) and `double?`
// (interface property) while probing this fix; filed as #2888.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A class with a block-form `decimal` property, a `decimal` indexer, and an
// interface-dispatched `decimal` property over one shared backing field, so a
// test can read the stored value back directly.
const char* kBox2883 =
    "public interface IHasP2883 "
    "  property P: decimal; "
    "end IHasP2883; "
    "public class Box2883: IHasP2883 "
    "  public _v: decimal := 0m; "
    "  public property P: decimal "
    "    get => _v; "
    "    set => do _v := value; end; "
    "  end; "
    "  public property [i: int]: decimal "
    "    get => _v; "
    "    set => do _v := value; end; "
    "  end; "
    "end Box2883; ";

std::string program2883(const std::string& body) {
    return std::string("module M2883; ") + kBox2883 +
           "public class Main2883 "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end Main2883;";
}

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — the issue's repro A shape: a block-form `decimal` property under each
// of §6.6's four arithmetic compound operators. §6.6, verbatim: "the statement
// `x op= e` is equivalent to `x := x op e` with the target evaluated **once**".
// Pre-fix: the compile ABORTED (IR verification failed). Post-fix: 9 —
// ((10 + 2.5) - 0.5) * 3 / 4.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, property_four_ops_6_6) {
    BEZEL_EXPECT_EQ(runAotOut(program2883(
        "    using var d := new Box2883(); "
        "    d._v := 10m; "
        "    d.P += 2.5m; "
        "    d.P -= 0.5m; "
        "    d.P *= 3m; "
        "    d.P /= 4m; "
        "    Console.WriteLine(d._v.ToString()); "
        "    return 0; ")), "9");
}

// ===========================================================================
// T2 — §17.6: "For built-in numeric types, `x++` is equivalent to `x := x + 1`
// (and likewise `x--` to `x := x - 1`)" — on a block-form `decimal` property.
// This is the spelling that failed on the SYNTHESIZED `1` (the `i33 1`
// constant), one step before emitCompoundOp. Post-fix, from 12.5:
// `++` -> 13.5, `--` -> 12.5, `--` -> 11.5.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, property_incr_decr_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(program2883(
        "    using var d := new Box2883(); "
        "    d._v := 12.5m; "
        "    d.P++; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    d.P--; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    d.P--; "
        "    Console.WriteLine(d._v.ToString()); "
        "    return 0; ")), "13.5|12.5|11.5");
}

// ===========================================================================
// T3 — the issue's repro B shape: a `decimal` INDEXER, both the compound and
// the increment spelling (§17.6's "Increment/decrement on indexer slots"
// desugars to a read-modify-write through the indexer). Post-fix: 9 then 10
// then 9.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, indexer_ops_and_incr_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(program2883(
        "    using var d := new Box2883(); "
        "    d._v := 10m; "
        "    d[0] += 2.5m; "
        "    d[0] -= 0.5m; "
        "    d[0] *= 3m; "
        "    d[0] /= 4m; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    d[0]++; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    d[0]--; "
        "    Console.WriteLine(d._v.ToString()); "
        "    return 0; ")), "9|10|9");
}

// ===========================================================================
// T4 — the arm the issue recorded as INFERRED-not-reproduced: a VIRTUAL /
// INTERFACE `decimal` property compound (the #481 arm of `lowerStmt`). It is
// real — reproduced before the fix with the same
// `llvm.sadd.with.overflow.s_bz.decimals` abort. The `++` spelling on the same
// receiver already worked (lowerVirtualPropertyIncrDecr has its own decimal
// branch); it is asserted here as the control that it still does.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, virtual_property_compound_23_3) {
    BEZEL_EXPECT_EQ(runAotOut(program2883(
        "    using var d := new Box2883(); "
        "    d._v := 10m; "
        "    var iv: IHasP2883 := d; "
        "    iv.P += 2.5m; "
        "    iv.P *= 2m; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    iv.P++; "
        "    Console.WriteLine(d._v.ToString()); "
        "    return 0; ")), "25|26");
}

// ===========================================================================
// T5 — §6.4, verbatim: "`decimal` arithmetic raises `OverflowException` on
// overflow and `DivideByZeroException` on division by zero." The fix routes the
// property/indexer compound through the same helper the local path uses, so
// both raises must survive the accessor round-trip — and the target must be
// LEFT UNCHANGED when the operation raises (§6.6 stores only after the op).
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, divide_by_zero_and_overflow_6_4) {
    // 1 = property `/= 0m` raised; 2 = the target kept its old value;
    // 4 = indexer `/= 0m` raised; 8 = `*=` at MaxValue overflowed. 15 = all.
    BEZEL_EXPECT_EQ(runFull(program2883(
        "    using var d := new Box2883(); "
        "    var score: int := 0; "
        "    d._v := 10m; "
        "    try "
        "      d.P /= 0m; "
        "    catch DivideByZeroException => score := score + 1; "
        "    end; "
        "    if d._v = 10m then score := score + 2; end if; "
        "    try "
        "      d[0] /= 0m; "
        "    catch DivideByZeroException => score := score + 4; "
        "    end; "
        "    d._v := decimal.MaxValue; "
        "    try "
        "      d.P *= 2m; "
        "    catch OverflowException => score := score + 8; "
        "    end; "
        "    return score; ")), 15);
}

// ===========================================================================
// T6 — §5.1: an integer right operand implicitly converts to `decimal`. The
// LOCAL path pre-coerces the RHS to the slot type before reaching
// emitCompoundOp; the property/indexer callers do not, so the new arm applies
// the widening itself. Without it the helper would store an `i32` into a
// `%bz.decimal` slot. Post-fix: 15 for both spellings.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, integer_rhs_widens_5_1) {
    BEZEL_EXPECT_EQ(runAotOut(program2883(
        "    using var d := new Box2883(); "
        "    d._v := 10m; "
        "    d.P += 5; "
        "    Console.Write(d._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    d._v := 10m; "
        "    d[0] += 5; "
        "    Console.WriteLine(d._v.ToString()); "
        "    return 0; ")), "15|15");
}

// ===========================================================================
// T7 — CONTROL (over-reach guard): the new arm is keyed on the LOWERED type
// being `%bz.decimal`, so every OTHER property/indexer compound must be
// bit-identical to before. int (checked integer arithmetic, §6.1), double
// (§6.3), char (§5.2's code-point narrowing on the compound write-back,
// #1961), and string concatenation (§4.6) all still round-trip through the
// accessor.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, non_decimal_properties_unchanged_6_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M2883b; "
        "public class Mixed2883 "
        "  public _i: int := 0; "
        "  public _d: double := 0.0; "
        "  public _c: char := 'a'; "
        "  public _s: string := \"\"; "
        "  public property I: int get => _i; set => do _i := value; end; end; "
        "  public property D: double get => _d; set => do _d := value; end; end; "
        "  public property C: char get => _c; set => do _c := value; end; end; "
        "  public property S: string get => _s; set => do _s := value; end; end; "
        "end Mixed2883; "
        "public class Main2883b "
        "  public static method Main(): int "
        "    using var m := new Mixed2883(); "
        "    m._i := 10; m.I += 5; m.I *= 2; m.I++; "
        "    m._d := 2.0; m.D += 0.5; m.D++; "
        "    m._c := 'a'; m.C += 2; m.C++; "
        "    m._s := \"ab\"; m.S += \"cd\"; "
        "    Console.WriteLine(m._i.ToString() + \"|\" + m._d.ToString() + "
        "                      \"|\" + m._c.ToString() + \"|\" + m._s); "
        "    return 0; "
        "  end Main; "
        "end Main2883b;"), "31|3.5|d|abcd");
}

// ===========================================================================
// T8 — CONTROL: a RECORD-backed `decimal` property compound and increment
// (§9.2 — a record's setter plain-stores). The new arm sits below the
// user-`operator` dispatch and above the string/fp/integer arms, so a record
// receiver takes the same route as a class one.
// ===========================================================================
BEZEL_TEST(codegen_decimal_property_compound_2883, record_backed_property_9_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M2883c; "
        "public record Rec2883 "
        "  public _v: decimal := 0m; "
        "  public property P: decimal get => _v; set => do _v := value; end; end; "
        "end Rec2883; "
        "public class Main2883c "
        "  public static method Main(): int "
        "    var r: Rec2883 := new Rec2883(); "
        "    r._v := 4m; "
        "    r.P += 1.5m; "
        "    Console.Write(r._v.ToString()); "
        "    Console.Write(\"|\"); "
        "    r.P++; "
        "    Console.WriteLine(r._v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Main2883c;"), "5.5|6.5");
}
