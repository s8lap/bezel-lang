// Spec-derived regression test for issue #2859 (INDEXER twin).
//
// §23.4 (Block-form properties — indexers) / §7.2 (Nullable value types) /
// §5.1 (Implicit conversions) / §6.6 (Compound assignments) / §12.1
// (Members are non-virtual by default — virtual dispatch), v0.240.
//
// THE DEFECT. Issue #2859 filed against the PROPERTY setter — `lowerVirtual-
// PropertySet` (LlvmBackend_Exprs.cpp) coerced the value with `coerceNumericToType`
// alone, which has no case for §7.2's tagged `{i8 hasValue; T}` slot, so a
// `property Q: int?` write `b.Q := 4` emitted a malformed IR call (`call void
// %pset(ptr, i32)` where `void(ptr, {i8, i32})` was declared) and `bezelc`
// aborted. Fix #2927 wrapped the value before the dispatched call.
//
// The issue's recorded-but-not-measured INFERENCE — "Also worth checking in
// the same pass: the INDEXER twins (`lowerIndexerGet` / `lowerIndexerSet` /
// `lowerIndexerCompoundCore`) build their element type the same way, so a
// `property [i: int]: int?` on a virtual indexer is likely to have the
// identical hole" — REPRODUCES on ca52982 with a virtual single-keyed
// indexer on a CLASS, an interface-typed receiver, and an imported cross-
// module indexer. All five setter sites in `lowerIndexerSet` (`!pd`/imported,
// itable, vtable) and all three compound sites in `lowerIndexerCompoundCore`
// (`!pd`/imported, itable, vtable) had the identical bare-`coerceNumericToType`
// form, compiled the malformed call, and failed IR verification with the
// same `Call parameter type does not match function signature! i32 <N> { i8,
// i32 } call void %idxset(...)` line the issue captured for the property
// case.
//
// THE FIX mirrors #2927 one-for-one — the SAME pair every other store into a
// nullable slot uses: `#2903`'s `coerceInitializerToSlot` (one definition of
// an initializer's slot width, with the nullable slot's PAYLOAD as the width
// to match, in both of §5.1's directions) then `#813`'s `coerceToNullableSlot`
// present-wrap (which also normalizes a `null` literal to the all-zero
// form). Applied at all six sites in LlvmBackend_Assign.cpp; both helpers
// are no-ops when `v` already IS the slot type, so callers that arrive with
// a tagged value (the §22.3 `ref`/`out` write-back, the §7.11 lifting arm of
// `emitCompoundOp`) are unaffected.
//
// Cases below exercise:
//   T1  class indexer, plain `r[i] := literal` (vtable setter path)
//   T2  class indexer, `r[i] := null` (the `null`-normalize half of #813)
//   T3  class indexer, `r[i] += 7` (§7.11 + vtable-setter compound path)
//   T4  class indexer, an already-tagged `int?` RHS — must NOT be re-wrapped
//       (the helpers' no-op invariant on identical types)
//   T5  interface-typed receiver, itable setter path
//   T6  interface compound, `i[i] += 7`
//
// Pre-fix: every T1..T6 aborts with IR verification failure. Post-fix: every
// T1..T6 returns the expected exit code.
//
// THE READ HALF. The getter side has no equivalent hole: `lowerIndexerGet`'s
// return type IS the getter's own ABI shape, and the value is loaded as-is.
// No wrapping on read.
//
// Related: #2859 (the property side), #2927 (the property fix this mirrors),
// #2905 (the §7.7/§6.6 `??=` path which pre-wraps for the same reason),
// #481 (the vtable indexer dispatch this completes).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §6.6 / §7.11 (v0.286, #3298): a read-modify-write compound over a nullable
// indexer slot is a compile error. Asserts the diagnostic, not just "did not
// run" — these are multi-declaration programs where an unrelated failure would
// otherwise pass.
bool rejectedCompound2859(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — class indexer, plain `r[i] := literal` through a VIRTUAL indexer on
// a derived receiver (so the vtable setter, not the base's own setter, is
// the one that runs). §23.4 / §12.1 / §7.2. Pre-fix: IR verification abort.
// Post-fix: 1.
// ===========================================================================
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, class_indexer_vtable_plain_set_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module I2859a; "
        "public class Box "
        "  public _v: int? := null; "
        "  public virtual property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b[0] := 7; "
        "    if (b._v ?? -1) = 7 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 1);
}

// ===========================================================================
// T2 — same shape, the `null`-literal half of `#813` (`coerceToNullableSlot`
// normalizes a `null` pointer to the `{i8 0, T 0}` all-zero form so the
// first `if` reads `b._v = null`). §7.2. Post-fix: 2.
// ===========================================================================
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, class_indexer_vtable_null_set_7_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module I2859b; "
        "public class Box "
        "  public _v: int? := null; "
        "  public virtual property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b1 := new Box(); "
        "    b1[0] := 11; "
        "    b1[3] := null; "
        "    if b1._v = null then acc := acc + 1; end; "
        "    using var b2 := new Box(); "
        "    b2[5] := null; "
        "    if b2._v = null then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T3 — class indexer, compound `r[i] += 7` (the §7.11 LIFTING arm of
// `emitCompoundOp` produces an already-tagged value, which the helper pair
// must re-wrap into the slot — see the `lowerIndexerCompoundCore` vtable
// compound site). §6.6 / §7.11 / §7.2 / §23.4. Post-fix: 1.
// ===========================================================================
// §6.6 / §7.11 (v0.286, #3298) — REJECTION. #2859's subject is the vtable
// dispatch of a nullable indexer SETTER, which the plain `b[0] := v` rows in
// this file still cover; the COMPOUND spelling is a compile error now, because
// §6.6 rejects a nullable target and an indexer slot is not narrowable storage
// under §7.4 at all.
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, class_indexer_vtable_compound_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound2859(
        "module I2859c; "
        "public class Box "
        "  public _v: int? := 1; "
        "  public virtual property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    b[0] += 7; "
        "    if (b._v ?? -1) = 8 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"));
}

// ===========================================================================
// T4 — class indexer, an already-tagged `int?` RHS — the helper pair must
// be a NO-OP on a value that already has the slot type (`v->getType() ==
// st->getElementType(1)` is false but `v->getType() == st` is true and #813
// guards on the latter). §7.2. Post-fix: 1.
// ===========================================================================
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, class_indexer_vtable_tagged_rhs_7_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module I2859d; "
        "public class Box "
        "  public _v: int? := null; "
        "  public virtual property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var src: int? := 6; "
        "    b[0] := src; "
        "    if (b._v ?? -1) = 6 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 1);
}

// ===========================================================================
// T5 — INTERFACE-typed receiver, itable setter path. §13 / §23.4 / §7.2.
// Pre-fix: IR verification abort. Post-fix: 2.
// ===========================================================================
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, interface_indexer_itable_set_13) {
    BEZEL_EXPECT_EQ(runProg(
        "module I2859e; "
        "public interface IHasIdx "
        "  property [k: int]: int?; "
        "end IHasIdx; "
        "public class Box: IHasIdx "
        "  public _v: int? := null; "
        "  public property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasIdx := b; "
        "    i[0] := 9; "
        "    if (b._v ?? -1) = 9 then acc := acc + 1; end; "
        "    i[1] := null; "
        "    if b._v = null then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T6 — INTERFACE compound, `i[i] += 7` on an interface-typed receiver (the
// §6.6 + itable compound site in `lowerIndexerCompoundCore`). §6.6 / §13
// / §23.4 / §7.11. Post-fix: 1.
// ===========================================================================
BEZEL_TEST(codegen_virtual_indexer_nullable_set_2859, interface_indexer_compound_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound2859(
        "module I2859f; "
        "public interface IHasIdx "
        "  property [k: int]: int?; "
        "end IHasIdx; "
        "public class Box: IHasIdx "
        "  public _v: int? := 2; "
        "  public property [k: int]: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasIdx := b; "
        "    i[0] += 5; "
        "    if (b._v ?? -1) = 7 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"));
}
