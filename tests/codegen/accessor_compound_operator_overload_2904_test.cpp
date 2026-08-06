// Spec-derived regression test for issue #2904.
//
// §25.1 (Overloadable operators) / §25.2 (Operator overloading restrictions) /
// §6.6 (Compound assignments) / §23.3 (Block-form properties) / §23.4
// (Indexers) / §12.1 (Virtual dispatch) / §17.6 (Increment and decrement),
// v0.240.
//
// THE DEFECT. A compound assignment to an ACCESSOR-MEDIATED target whose type
// declares a §25 `operator <op>` ABORTED the compile. Measured on c7404bf, all
// three with the same IR-verifier error — "Intrinsic has incorrect return type!
// ptr @llvm.sadd.with.overflow.s_record.Pairs":
//
//     b.PP  += one;   a block-form property        (#2904's own repro)
//     i.VP  += one;   a property through an INTERFACE
//     b[0]  += one;   a user indexer
//
// while the LOCAL, FIELD, AUTO-PROPERTY and heap-collection spellings of the
// very same statement were all correct.
//
// THE CAUSE — one rule, three hand-written copies, and a fan of callers with
// none. §25.1's not-overloadable list, verbatim: "Assignment and
// compound-assignment operators (`:=`, `+=`, `-=`, `*=`, `/=`, `&=`, `|=`,
// `^=`, `<<=`, `>>=`, `??=`) — Bezel has no compound-operator overload
// mechanism; the compound forms desugar mechanically to read-modify-write using
// the underlying binary operator's overload, with the evaluation counts and
// result typing of [§6.6](#sec-compound-assignments)." §25.2 "Resolution",
// verbatim: "the candidate set is the union of the `operator <op>` overloads
// declared on the **static types of both operands, including declarations
// inherited from their base classes** … Parameters match operands **in order**
// — there is no automatic commutation". `Emitter::
// emitCompoundOp` structurally CANNOT apply that rule — it sees only the two
// lowered LLVM values, and an overload is selected from the operands' SEMA
// types — so the lookup has to happen at the caller. It existed in exactly
// three of them: `lowerStmt`'s local compound arm (#610/#2156, the full
// in-order match) and the two heap-collection branches of
// `lowerCollectionIndexerCompound` (#626, a weaker element-type-only scan).
// Every accessor-mediated caller — `lowerPropertyCompound`, the #481
// virtual/interface arm, `lowerIndexerCompoundCore`'s three compound-value
// sites, `lowerImportedPropertyCompoundCore` — went straight to
// `emitCompoundOp`, whose integer tail handed the record struct to
// `llvm.sadd.with.overflow`.
//
// THE FIX gives the rule ONE definition: `findCompoundOperatorOverload` (the
// §25.2 in-order match, lifted verbatim out of the #610 arm, which is now the
// plain call) and `emitCompoundOpTyped` = `emitCompoundOp` + that arm. Every
// compound caller that has the operand types now uses it. The two
// heap-collection copies are replaced by the shared lookup as well — which also
// upgrades them to §25.2's in-order parameter match, which they never had —
// while keeping their own write-back, which must reuse the already-evaluated
// receiver/index (§17.6/§6.6's "evaluated exactly once").
//
// Every case asserts a VALUE and, for T4, the mechanically counted number of
// getter, setter and index-expression evaluations.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `Pair` with a §25 `operator +`; `Scale` with the #2156 shape — an operator
// declared on the RIGHT operand's type whose param0 is a PRIMITIVE.
const char* kTypes2904 =
    "public record Pair2904 "
    "  public a: int; "
    "  public b: int; "
    "  public static method operator +(x: Pair2904, y: Pair2904): Pair2904 "
    "    return new Pair2904(a := x.a + y.a, b := x.b + y.b); "
    "  end; "
    "end Pair2904; "
    "public record Scale2904 "
    "  public k: int; "
    "  public static method operator *(x: int, s: Scale2904): int "
    "    return x * s.k; "
    "  end; "
    "end Scale2904; "
    "public interface IHasP2904 "
    "  property VP: Pair2904; "
    "end IHasP2904; "
    "public class Box2904: IHasP2904 "
    "  public _p: Pair2904 := new Pair2904(a := 1, b := 2); "
    "  public _v: Pair2904 := new Pair2904(a := 1, b := 2); "
    "  public _x: Pair2904 := new Pair2904(a := 1, b := 2); "
    "  public _n: int := 6; "
    "  public property PP: Pair2904 "
    "    get => _p; "
    "    set => do _p := value; end; "
    "  end; "
    "  public property VP: Pair2904 "
    "    get => _v; "
    "    set => do _v := value; end; "
    "  end; "
    "  public property NP: int "
    "    get => _n; "
    "    set => do _n := value; end; "
    "  end; "
    "  public property [i: int]: Pair2904 "
    "    get => _x; "
    "    set => do _x := value; end; "
    "  end; "
    "end Box2904; ";

int run2904(const std::string& body) {
    auto r = runAot(std::string("module M2904; ") + kTypes2904 +
                    "public class Main2904 "
                    "  public static method Main(): int " +
                    body +
                    "  end Main; "
                    "end Main2904;");
    return r.ran ? r.exitCode : -999;
}

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — #2904's own repro. §25.1: `b.PP += one` desugars to
// `b.PP := Pair.operator+(b.PP, one)` — getter, operator call, setter.
// `Pair(1,2) + Pair(1,1)` is `Pair(2,3)`, so `a*10 + b` is 23.
// Pre-fix: the compile ABORTED ("Intrinsic has incorrect return type! ptr
// @llvm.sadd.with.overflow.s_record.Pair2904s").
// (The issue text says 22; 2*10 + 3 = 23 — the arithmetic in the report was
// off by one, verified by running the desugaring by hand.)
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904, block_form_property_25_1) {
    BEZEL_EXPECT_EQ(run2904(
        "    using var b := new Box2904(); "
        "    var one := new Pair2904(a := 1, b := 1); "
        "    b.PP += one; "
        "    return b.PP.a * 10 + b.PP.b; "), 23);
}

// ===========================================================================
// T2 — the two accessor spellings the issue INFERRED but did not probe, both
// measured to fail the same way on c7404bf: a property reached through an
// INTERFACE (itable dispatch, the #481 arm) and a USER INDEXER (§23.4,
// `lowerIndexerCompoundCore`). Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904, interface_and_indexer_25_1) {
    BEZEL_EXPECT_EQ(run2904(
        "    var acc := 0; "
        "    using var b := new Box2904(); "
        "    var one := new Pair2904(a := 1, b := 1); "
        "    var iv: IHasP2904 := b; "
        "    iv.VP += one; "
        "    if b._v.a = 2 and b._v.b = 3 then acc := acc + 1; end; "
        "    b[0] += one; "
        "    if b._x.a = 2 and b._x.b = 3 then acc := acc + 2; end; "
        "    return acc; "), 3);
}

// ===========================================================================
// T3 — §25.2's BOTH-operands candidate set through an accessor. #2156's shape:
// `operator *(int, Scale)` is declared on the RIGHT operand's type and its
// param0 is a PRIMITIVE, so a search of the target's type alone finds nothing.
// The local spelling has had this since #2156; the property spelling gets it
// from the shared lookup. `6 * Scale(7)` is 42.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904, rhs_declared_operator_25_2) {
    BEZEL_EXPECT_EQ(run2904(
        "    using var b := new Box2904(); "
        "    var s := new Scale2904(k := 7); "
        "    b.NP *= s; "
        "    return b.NP; "), 42);
}

// ===========================================================================
// T4 — §6.6's EVALUATION COUNTS, counted mechanically rather than trusted.
// §6.6, verbatim: "every constituent of the left-hand side's lvalue chain
// (grammar A.6 `LValue`) is evaluated exactly once, left to right … when the
// final segment is accessor-mediated (a property or an indexer), its getter and
// setter are each invoked exactly once". The accessor bodies bump counters and
// the index is a counter-returning call, so a re-evaluated receiver, index,
// getter or setter shows up as a wrong count — not as a wrong value.
// Post-fix: 31.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904, single_evaluation_6_6) {
    BEZEL_EXPECT_EQ(runProg(
        "module M2904e; "
        "public record Pair "
        "  public a: int; "
        "  public b: int; "
        "  public static method operator +(x: Pair, y: Pair): Pair "
        "    return new Pair(a := x.a + y.a, b := x.b + y.b); "
        "  end; "
        "end Pair; "
        "public class Ctr "
        "  public static Idx: int := 0; "
        "  public static Gets: int := 0; "
        "  public static Sets: int := 0; "
        "  public static method NextIdx(): int "
        "    Ctr.Idx := Ctr.Idx + 1; "
        "    return 0; "
        "  end NextIdx; "
        "end Ctr; "
        "public class Box "
        "  public _p: Pair := new Pair(a := 1, b := 2); "
        "  public property PP: Pair "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _p; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _p := value; end; "
        "  end; "
        "  public property [i: int]: Pair "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _p; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _p := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var one := new Pair(a := 1, b := 1); "
        "    using var b := new Box(); "
        "    b.PP += one; "
        "    if Ctr.Gets = 1 and Ctr.Sets = 1 then acc := acc + 1; end; "
        "    if b._p.a = 2 and b._p.b = 3 then acc := acc + 2; end; "
        "    Ctr.Gets := 0; Ctr.Sets := 0; "
        "    b[Ctr.NextIdx()] += one; "
        "    if Ctr.Idx = 1 then acc := acc + 4; end; "
        "    if Ctr.Gets = 1 and Ctr.Sets = 1 then acc := acc + 8; end; "
        "    if b._p.a = 3 and b._p.b = 4 then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 31);
}

// ===========================================================================
// T5 — OVER-ACCEPTANCE CONTROL. `++` on a record-typed property must stay a
// sourced REJECT. §17.6 identifies `x++` with `x := x + 1` only "For built-in
// numeric types", and §25.2 gives a record its own route — "`++` and `--` are
// postfix-only and statement-only. There is no separate prefix overload; the
// single overload is used in the one permitted position" — i.e. an
// `operator ++` the record must DECLARE. `Pair2904` declares `operator +` and
// no `operator ++`, so this must be rejected. The fix widens what an accessor
// COMPOUND may dispatch to; this pins that it did not widen the INCREMENT
// spelling with it (a `+` overload must not become a `++`), and that the reject
// is a diagnostic, not an IR-verifier abort.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904, record_property_increment_rejected_17_6) {
    auto r = runAot(std::string("module M2904f; ") + kTypes2904 +
                    "public class Main2904f "
                    "  public static method Main(): int "
                    "    using var b := new Box2904(); "
                    "    b.PP++; "
                    "    return b.PP.a; "
                    "  end Main; "
                    "end Main2904f;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic TEXT matters: "did not run" alone would also be satisfied
    // by the IR-verifier abort this fix exists to remove.
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§17.6") != std::string::npos) cited = true;
    BEZEL_EXPECT_TRUE(cited);
}

// ===========================================================================
// T6 — OVER-ACCEPTANCE CONTROL for the two heap-collection branches, whose
// hand-written lookup this fix REPLACED with the shared one. `Array<Pair>` and
// `Dictionary<K,Pair>` compound assignment was already correct on c7404bf
// (measured), so it must be bit-identical after: the shared lookup adds §25.2's
// in-order parameter match, which must still SELECT the matching overload.
// Passes both with and without the fix, by construction. Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904,
           heap_collection_element_unchanged_control) {
    BEZEL_EXPECT_EQ(run2904(
        "    var acc := 0; "
        "    var one := new Pair2904(a := 1, b := 1); "
        "    using var arr := new Array<Pair2904>(); "
        "    arr.Add(new Pair2904(a := 1, b := 2)); "
        "    arr[0] += one; "
        "    if arr[0].a = 2 and arr[0].b = 3 then acc := acc + 1; end; "
        "    using var d := new Dictionary<int, Pair2904>(); "
        "    d[7] := new Pair2904(a := 1, b := 2); "
        "    d[7] += one; "
        "    if d[7].a = 2 and d[7].b = 3 then acc := acc + 2; end; "
        "    return acc; "), 3);
}

// ===========================================================================
// T7 — OVER-ACCEPTANCE CONTROL for the non-overload path. The fix inserts a
// lookup in front of `emitCompoundOp` at every accessor caller; an ORDINARY
// numeric compound through the same callers must be untouched — including the
// §6.6/§5.2 CHECKED narrow-integer write-back (`byte += 200` on a `byte(100)`
// target raises `OverflowException`, never a wraparound), which lives past the
// point the new arm returns from. Passes both ways by construction. Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_accessor_compound_operator_overload_2904,
           builtin_numeric_accessor_compound_unchanged_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M2904g; "
        "public interface IHasN "
        "  property VN: int; "
        "end IHasN; "
        "public class Box: IHasN "
        "  public _n: int := 10; "
        "  public _y: byte := 100; "
        "  public _q: int := 10; "
        "  public property NP: int "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "  public property BP: byte "
        "    get => _y; "
        "    set => do _y := value; end; "
        "  end; "
        "  public property VN: int "
        "    get => _q; "
        "    set => do _q := value; end; "
        "  end; "
        "  public property [i: int]: int "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b.NP += 5; "
        "    if b._n = 15 then acc := acc + 1; end; "
        "    b[0] += 5; "
        "    if b._n = 20 then acc := acc + 2; end; "
        "    var iv: IHasN := b; "
        "    iv.VN += 5; "
        "    if b._q = 15 then acc := acc + 4; end; "
        "    try b.BP += 200; catch OverflowException => acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}
