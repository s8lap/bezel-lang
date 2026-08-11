// Spec-derived regression test for issue #2905.
//
// §7.7 (Null-coalescing `??` and `??=`) / §6.6 (Compound assignments) / §23.3
// (Block-form properties) / §23.4 (Indexers) / §12.1 (Virtual dispatch) / §7.2
// (Nullable value types), v0.240.
//
// THE DEFECT. `target ??= e` on an ACCESSOR-MEDIATED target was SILENTLY
// DROPPED — no diagnostic, no crash, the write simply never happened. Measured
// on c7404bf, every one of these:
//
//     b.NP ??= 77;      a block-form `int?` property   (#2905's own repro:
//                       exit 5 where 3 is correct — the local spelling beside
//                       it was right and the property stayed `null`)
//     b.SP ??= "hi";    a block-form `string?` property
//     b[0] ??= 11;      a user indexer
//     arr[0] ??= 21;    an `Array<int?>` element
//     d[3]  ??= 22;     a `Dictionary<int,int?>` element
//
// and a SIXTH, worse than a drop — a property reached through an INTERFACE:
//
//     i.IP ??= 9;       wrote 9 into the receiver's FIELD INDEX 0. With
//                       `_z`, `_n`, `_v` declared in that order and `IP` backed
//                       by `_v`, the measured result was `z=9, n=-1, v=-1`.
//
// THE CAUSE. §6.6, verbatim: "**`??=`** is not a read-modify-write: `x ??= e`
// assigns `e` only when `x`'s current value is null, with the typing and
// ownership rules of [§7.7]. The LHS chain is evaluated once as above, the
// getter once; `e` is evaluated — and the setter invoked — only when the
// assignment actually occurs." That is a CONTROL-FLOW construct, so no
// `emitCompoundOp` arm can express it — and every read-modify-write handler
// therefore declined `??=` (each ends in an `emitCompoundOp` call with the
// operator `??`, which matches no arm and returns null). What was left was a
// bare `lvalueAddr` gate: `if (!addr) return false;`. `lvalueAddr` returns null
// for a property or an indexer, which have no backing slot — hence the drop —
// and for an INTERFACE property it returned an address that was simply WRONG.
//
// THE FIX gives `??=` the accessor path its five sibling operators already had.
// The getter read runs once; the null test branches; and on the null branch ONLY
// `e` is lowered and the store is dispatched through the SAME ordered helper
// chain the plain `:=` arm uses — so the property gated on is "can the `:=`
// spelling write this target?", not an enumeration of accessor kinds. A
// virtual/interface property is routed to that chain BEFORE `lvalueAddr` (§12.1
// requires runtime dispatch, so an addressed store is wrong for it by
// construction). If no helper claims the store, an internal diagnostic is now
// reported — the write is never silently dropped again.
//
// Every case asserts a VALUE, and T3 asserts the §6.6 evaluation counts
// MECHANICALLY (counter-bumping accessor bodies and a counter-returning RHS),
// because "not dropped" and "correct" are different claims and this defect
// passes any test that only checks the program does not crash.
//
// KNOWN RESIDUAL, filed separately: for the INDEXER spellings the LHS chain's
// index expression is evaluated TWICE (the getter read and the setter write
// each lower it), where §6.6 requires once — measured `idx=2`. That is a
// property of reading through `lowerExpr` and writing through `lowerIndexerSet`;
// `lowerIndexerCompoundCore` caches receiver and indices for `OP=` and is the
// model for closing it. T3 pins the getter/setter/RHS counts, which ARE correct,
// and deliberately does not assert the index count.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — #2905's own repro, unchanged. The LOCAL spelling is correct and is kept
// beside the property one, so a green result cannot come from the property
// write being skipped: bit 4 asserts the property is NO LONGER null.
// Pre-fix: 5 (local correct, property never written, still null).
// Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, block_form_property_7_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2905a; "
        "public class Box "
        "  public _n: int? := null; "
        "  public property NP: int? "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var loc: int? := null; "
        "    loc ??= 5; "
        "    if (loc ?? -1) = 5 then acc := acc + 1; end; "
        "    using var b := new Box(); "
        "    b.NP ??= 77; "
        "    if (b.NP ?? -1) = 77 then acc := acc + 2; end; "
        "    if b.NP = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T2 — the whole accessor family the issue INFERRED but did not probe, each
// measured DROPPED on c7404bf: a block-form nullable-REFERENCE property (a bare
// `ptr` slot, not §7.2's tagged struct), an INTERFACE property, a user indexer,
// an `Array<int?>` element and a `Dictionary<int,int?>` element — plus a
// nullable RECORD property, whose payload is an AGGREGATE.
// Post-fix: 63.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, accessor_family_6_6) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2905b; "
        "public record Pt "
        "  public x: int; "
        "end Pt; "
        "public interface IHasQ "
        "  property IP: int?; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _s: string? := null; "
        "  public _v: int? := null; "
        "  public _x: int? := null; "
        "  public _p: Pt? := null; "
        "  public property SP: string? "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "  public property IP: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "  public property PP: Pt? "
        "    get => _p; "
        "    set => do _p := value; end; "
        "  end; "
        "  public property [i: int]: int? "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b.SP ??= \"hi\"; "
        "    if (b.SP ?? \"\") = \"hi\" then acc := acc + 1; end; "
        "    var i: IHasQ := b; "
        "    i.IP ??= 9; "
        "    if (b._v ?? -1) = 9 then acc := acc + 2; end; "
        "    b[0] ??= 11; "
        "    if (b._x ?? -1) = 11 then acc := acc + 4; end; "
        "    b.PP ??= new Pt(x := 6); "
        "    var q := b._p!; "
        "    if q.x = 6 then acc := acc + 8; end; "
        "    using var arr := new Array<int?>(); "
        "    arr.Add(null); "
        "    arr[0] ??= 21; "
        "    if (arr[0] ?? -1) = 21 then acc := acc + 16; end; "
        "    using var d := new Dictionary<int, int?>(); "
        "    d[3] := null; "
        "    d[3] ??= 22; "
        "    if (d[3] ?? -1) = 22 then acc := acc + 32; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 63);
}

// ===========================================================================
// T3 — §6.6's evaluation rule for `??=`, counted MECHANICALLY. Verbatim: "The
// LHS chain is evaluated once as above, the getter once; `e` is evaluated — and
// the setter invoked — only when the assignment actually occurs." So:
//   * target NULL      → getter 1, RHS 1, setter 1;
//   * target NON-NULL  → getter 1, RHS 0, setter 0.
// The second half is the half a naive `select(isNull, e, cur)` implementation
// would fail: it computes the right VALUE while running `e` and the setter
// unconditionally. (The indexer's index-expression count is the known residual
// noted in this file's header and is deliberately not asserted here.)
// Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, evaluation_counts_6_6) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2905c; "
        "public class Ctr "
        "  public static Gets: int := 0; "
        "  public static Sets: int := 0; "
        "  public static Rhs: int := 0; "
        "  public static method Val(): int "
        "    Ctr.Rhs := Ctr.Rhs + 1; "
        "    return 77; "
        "  end Val; "
        "end Ctr; "
        "public class Box "
        "  public _n: int? := null; "
        "  public property NP: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _n; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _n := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b.NP ??= Ctr.Val(); "
        "    if Ctr.Gets = 1 and Ctr.Sets = 1 and Ctr.Rhs = 1 "
        "      then acc := acc + 1; end; "
        "    if (b._n ?? -1) = 77 then acc := acc + 2; end; "
        "    Ctr.Gets := 0; Ctr.Sets := 0; Ctr.Rhs := 0; "
        "    b.NP ??= Ctr.Val(); "
        "    if Ctr.Gets = 1 and Ctr.Sets = 0 and Ctr.Rhs = 0 "
        "      then acc := acc + 4; end; "
        "    if (b._n ?? -1) = 77 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T4 — the INTERFACE property writes through the SETTER, not through a field
// address. §12.1: "A method, property, or indexer is non-virtual unless marked
// `virtual`" — an interface member is dispatched on the runtime type, so the
// getter and setter accessors must run. Pre-fix `lvalueAddr` resolved
// `i.IP` to the receiver's FIELD INDEX 0 and the write landed there: measured
// `z=9, n=-1, v=-1` where `v=9` is correct. The three separate fields make the
// wrong-slot write observable rather than accidentally right (with a single
// field it "passes" while never running the setter at all).
// Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, interface_property_slot_12_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2905d; "
        "public interface IHasQ "
        "  property IP: int?; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _z: int? := null; "
        "  public _n: int? := null; "
        "  public _v: int? := null; "
        "  public property NP: int? "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "  public property IP: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasQ := b; "
        "    i.IP ??= 9; "
        "    if (b._v ?? -1) = 9 then acc := acc + 1; end; "
        "    if b._z = null then acc := acc + 2; end; "
        "    if b._n = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T5 — §7.7's ownership rule for the assigned value, under ASan/LSan. §6.6
// routes `??=` through "the typing and ownership rules of §7.7", and the RHS
// here is a +1 STRING PRODUCER evaluated inside the null branch: run in a loop,
// exactly one of the iterations must assign it and the rest must not evaluate it
// at all, with the class field's single reference released once at disposal.
// A leak (the producer's +1 orphaned) or a double-release flips `.ran`.
// Post-fix: 1.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, owned_rhs_no_leak_7_7) {
    auto r = runAotChecked(
        "module N2905e; "
        "public class Box "
        "  public _s: string? := null; "
        "  public property SP: string? "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var i := 0; "
        "    while i < 20 do "
        "      b.SP ??= \"v\" + i.ToString(); "
        "      i := i + 1; "
        "    end; "
        "    if (b.SP ?? \"\") = \"v0\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T6 — OVER-ACCEPTANCE CONTROL. The fix widens what `??=` may write, so every
// spelling that ALREADY worked must be bit-identical: a LOCAL, a plain FIELD, an
// AUTO-property (accessor-mediated in the source but backed by a field slot, so
// it keeps the addressed path), a nullable REFERENCE local, and — the case that
// matters most — a target that is NOT null, which must keep its value and must
// not be re-assigned. Passes both with and without the fix, by construction.
// Post-fix: 31.
// ===========================================================================
BEZEL_TEST(codegen_accessor_null_coalescing_assign_2905, addressed_targets_unchanged_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2905f; "
        "public class Box "
        "  public _f: int? := null; "
        "  public property AP: int? := null; "
        "  public _g: int? := 3; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var loc: int? := null; "
        "    loc ??= 5; "
        "    if (loc ?? -1) = 5 then acc := acc + 1; end; "
        "    var sloc: string? := null; "
        "    sloc ??= \"q\"; "
        "    if (sloc ?? \"\") = \"q\" then acc := acc + 2; end; "
        "    using var b := new Box(); "
        "    b._f ??= 7; "
        "    if (b._f ?? -1) = 7 then acc := acc + 4; end; "
        "    b.AP ??= 8; "
        "    if (b.AP ?? -1) = 8 then acc := acc + 8; end; "
        "    b._g ??= 99; "
        "    if (b._g ?? -1) = 3 then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 31);
}
