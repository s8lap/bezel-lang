// Spec-derived regression test for issue #2929.
//
// §6.6 (Compound assignments) / §7.7 (Null-coalescing `??` and `??=`) / §23.4
// (Indexers) / §23.3 (Block-form properties) / §17.6, v0.240.
//
// THE RULE. §6.6's `??=` paragraph, verbatim: "**`??=`** is not a
// read-modify-write: `x ??= e` assigns `e` only when `x`'s current value is
// null, with the typing and ownership rules of [§7.7]. **The LHS chain is
// evaluated once as above**, the getter once; `e` is evaluated — and the setter
// invoked — only when the assignment actually occurs."
//
// "as above" is this clause, also verbatim: "every constituent of the
// left-hand side's lvalue chain (grammar A.6 `LValue`) is evaluated exactly
// once, left to right: the base identifier or `this`, each mid-chain member hop
// (a property hop's getter is invoked once as part of locating the target), and
// every index expression in the chain".
//
// THE DEFECT. `lowerAccessorNullCoalescingAssign` (#2905) reads through
// `lowerExpr(*as.target)` and writes through a setter helper that re-lowers the
// target's own subexpressions FROM THE AST, so every constituent of the chain
// ran TWICE. MEASURED on 10bd6d3 (the parent commit), with counter-bumping
// accessor bodies, a counter-returning index and a counter-returning RHS:
//
//     b[Ctr.NextIdx()] ??= Ctr.Val()       idx=2   (§6.6 requires 1)
//     h.Inner.NP       ??= Ctr.Val()       inner=2 (the mid-chain property hop)
//     h.Inner[Ctr.NextIdx()] ??= Ctr.Val() inner=2, idx=2
//
// with `gets=1, sets=1, rhs=1` already correct in each. The mid-chain hop is
// wider than the filing, which named only the index: #2905 had recorded the
// receiver re-lowering as harmless on the grounds that A.6 forbids a
// method-call base — but a PROPERTY hop is not a method call, and §6.6 names it
// explicitly.
//
// THE FIX evaluates each constituent ONCE and pins it in `exprValueOverride_`,
// the memo `lowerExpr` consults first — the same mechanism §22.3's accessor
// `ref`/`out` bracket (`emitAccessorRefArgCopyIn`) already uses for the
// identical requirement, so the getter read and the setter's internal
// re-lowering share one value. Pins are dropped at the join, and an expression
// already pinned by an enclosing bracket is left alone.
//
// WHY COUNTED, NOT INSPECTED. An evaluation-count violation produces the RIGHT
// VALUE — nothing about `b[Next()] ??= v` looks wrong from the outside — so a
// test that asserts the stored value alone passes while the side effect runs
// twice. Every case here therefore counts the constituents with static
// counters and asserts the count, and pins the stored value beside it so a
// "count is 1 because nothing ran" regression cannot pass either.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The counter class every case below shares.
const char* kCtr =
    "public class Ctr "
    "  public static Idx: int := 0; "
    "  public static Idx2: int := 0; "
    "  public static Inner: int := 0; "
    "  public static Gets: int := 0; "
    "  public static Sets: int := 0; "
    "  public static Rhs: int := 0; "
    "  public static method Val(): int "
    "    Ctr.Rhs := Ctr.Rhs + 1; "
    "    return 77; "
    "  end Val; "
    "  public static method NextIdx(): int "
    "    Ctr.Idx := Ctr.Idx + 1; "
    "    return 0; "
    "  end NextIdx; "
    "  public static method NextIdx2(): int "
    "    Ctr.Idx2 := Ctr.Idx2 + 1; "
    "    return 0; "
    "  end NextIdx2; "
    "end Ctr; ";

}  // namespace

// ===========================================================================
// T1 — #2929's own repro: a user indexer with a counter-returning index.
// §6.6's four counts are all asserted, so a fix that made the index run once by
// dropping the write (the PRE-#2905 state, which also read `idx=1`) fails on
// the setter count and the stored value.
// Pre-fix: 14 (idx=2).  Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_null_coalescing_assign_chain_once_2929, index_expression_once_6_6) {
    BEZEL_EXPECT_EQ(runProg(
        std::string("module Q2929a; ") + kCtr +
        "public class Box "
        "  public _x: int? := null; "
        "  public property [i: int]: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _x; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _x := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b[Ctr.NextIdx()] ??= Ctr.Val(); "
        "    if Ctr.Idx = 1 then acc := acc + 1; end; "
        "    if Ctr.Gets = 1 then acc := acc + 2; end; "
        "    if Ctr.Sets = 1 then acc := acc + 4; end; "
        "    if Ctr.Rhs = 1 and (b._x ?? -1) = 77 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T2 — the MID-CHAIN PROPERTY HOP, for both the property and the indexer
// spelling of the final segment. §6.6: "each mid-chain member hop (a property
// hop's getter is invoked once as part of locating the target)".
// Pre-fix: 0 (inner=2 in both halves; and idx=2 in the second).
// Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_null_coalescing_assign_chain_once_2929, midchain_property_hop_once_6_6) {
    BEZEL_EXPECT_EQ(runProg(
        std::string("module Q2929b; ") + kCtr +
        "public class Leaf "
        "  public _x: int? := null; "
        "  public property NP: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _x; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _x := value; end; "
        "  end; "
        "  public property [i: int]: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _x; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _x := value; end; "
        "  end; "
        "end Leaf; "
        "public class Holder "
        "  public owned _leaf: Leaf; "
        "  public constructor() "
        "    _leaf := new Leaf(); "
        "  end constructor; "
        "  public property Inner: Leaf "
        "    get => do Ctr.Inner := Ctr.Inner + 1; return _leaf; end; "
        "  end; "
        "end Holder; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var h1 := new Holder(); "
        "    h1.Inner.NP ??= Ctr.Val(); "
        "    if Ctr.Inner = 1 and Ctr.Gets = 1 and Ctr.Sets = 1 and "
        "       Ctr.Rhs = 1 and (h1._leaf._x ?? -1) = 77 then acc := acc + 1; end; "
        "    Ctr.Inner := 0; Ctr.Gets := 0; Ctr.Sets := 0; Ctr.Rhs := 0; "
        "    using var h2 := new Holder(); "
        "    h2.Inner[Ctr.NextIdx()] ??= Ctr.Val(); "
        "    if Ctr.Inner = 1 and Ctr.Idx = 1 and Ctr.Gets = 1 and "
        "       Ctr.Sets = 1 and Ctr.Rhs = 1 and "
        "       (h2._leaf._x ?? -1) = 77 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T3 — the NON-NULL branch. §6.6: "`e` is evaluated — and the setter invoked —
// only when the assignment actually occurs." The chain still runs exactly once
// (it must, to locate the target and call the getter), but `e` and the setter
// must not run at all, and the existing value must survive. This half was
// already correct before the fix — the setter path is where the re-lowering
// lived — and is kept as the control that the pinning did not turn the
// short-circuit into an unconditional store.
// Pre-fix: 3.  Post-fix: 3. A deliberate always-green control.
// ===========================================================================
BEZEL_TEST(codegen_null_coalescing_assign_chain_once_2929, non_null_target_short_circuits_7_7) {
    BEZEL_EXPECT_EQ(runProg(
        std::string("module Q2929c; ") + kCtr +
        "public class Box "
        "  public _x: int? := null; "
        "  public property [i: int]: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _x; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _x := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b._x := 5; "
        "    b[Ctr.NextIdx()] ??= Ctr.Val(); "
        "    if Ctr.Idx = 1 and Ctr.Gets = 1 then acc := acc + 1; end; "
        "    if Ctr.Sets = 0 and Ctr.Rhs = 0 and (b._x ?? -1) = 5 then "
        "      acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T4 — EVERY index of a MULTI-index indexer, and the built-in heap-collection
// element spellings, which reach different setter helpers
// (`lowerCollectionIndexerSet`) than the user indexer above and re-lowered
// their key just the same.
//   bit 1  a two-index user indexer: both index expressions once;
//   bit 2  an `Array<int?>` element with a counter-returning index;
//   bit 4  a `Dictionary<int,int?>` element with a counter-returning key.
// Pre-fix: 0.  Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_null_coalescing_assign_chain_once_2929, every_index_and_collections_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        std::string("module Q2929d; ") + kCtr +
        "public class Box "
        "  public _x: int? := null; "
        "  public property [i: int, j: int]: int? "
        "    get => do Ctr.Gets := Ctr.Gets + 1; return _x; end; "
        "    set => do Ctr.Sets := Ctr.Sets + 1; _x := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b[Ctr.NextIdx(), Ctr.NextIdx2()] ??= Ctr.Val(); "
        "    if Ctr.Idx = 1 and Ctr.Idx2 = 1 and Ctr.Gets = 1 and "
        "       Ctr.Sets = 1 and (b._x ?? -1) = 77 then acc := acc + 1; end; "
        "    Ctr.Idx := 0; Ctr.Rhs := 0; "
        "    using var arr := new Array<int?>(); "
        "    arr.Add(null); "
        "    arr[Ctr.NextIdx()] ??= Ctr.Val(); "
        "    if Ctr.Idx = 1 and Ctr.Rhs = 1 and (arr[0] ?? -1) = 77 then "
        "      acc := acc + 2; end; "
        "    Ctr.Idx := 0; Ctr.Rhs := 0; "
        "    using var d := new Dictionary<int, int?>(); "
        "    d[0] := null; "
        "    d[Ctr.NextIdx()] ??= Ctr.Val(); "
        "    if Ctr.Idx = 1 and Ctr.Rhs = 1 and (d[0] ?? -1) = 77 then "
        "      acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}
