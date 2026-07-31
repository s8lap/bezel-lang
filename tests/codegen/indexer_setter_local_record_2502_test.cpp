// fix #2502 (§9.2/§23.4, v0.236): a LOCAL (non-imported) record's read-write
// INDEXER set through a named variable mutated a THROWAWAY COPY, not the
// variable's own storage — the write silently did not persist. No crash, no
// diagnostic: a silent-wrong-result bug.
//
// Root cause: `Emitter::lowerIndexerSet`'s inline-body branch
// (LlvmBackend_Assign.cpp) resolved the receiver via
//     llvm::Value* obj = lowerExpr(*ix.target);
//     if (!obj->getType()->isPointerTy()) { spill obj to a FRESH alloca; }
// `lowerExpr` on a plain identifier referencing a record-typed local LOADS
// the record's CURRENT VALUE — a byte-for-byte copy (records are by-value,
// §9.2), not a reference to the variable's own storage. The "spill" then
// allocated a brand-new, throwaway alloca and stored the COPY into it —
// `this` inside the setter body pointed at that throwaway slot. The
// setter's write (`_v := value - i;`) mutated the throwaway copy; the
// original local variable's own alloca was never touched. Confirmed on
// origin/main by direct probe (this file's own T1 repro): `r[5] := 9;
// return r[2];` returned 2 (the untouched initial value _v=0, so
// r[2] = 0 + 2), not 6 — the SET compiled, linked, and ran clean while
// silently doing nothing.
//
// This is the pre-existing, same-module sibling of the identical trap
// #2489's lane hit (and fixed, in the SAME function's imported `!pd`
// branch, plus `lowerImportedPropertySetValue`) while implementing the
// cross-module analog — see f4caed1. The fix mirrors that commit exactly:
// prefer `lvalueAddr` (the receiver's OWN storage address, resolved via
// `localSlot`/`fieldAddr`/etc.) over `lowerExpr` (a copy); fall back to
// `lowerExpr` + spill only when the receiver expression is genuinely NOT an
// addressable place (a fresh temporary — no "original" exists to preserve
// in that case, so a spilled copy is legal-but-meaningless, not unsound).
//
// T1/T2: the silent-no-op repro (public/private backing) — pre-fix returns
// the UNCHANGED read value; post-fix returns the value computed from the
// ACTUAL write. T3: a local CLASS indexer control (classes were never
// affected — a class reference already lowers to a pointer — regression
// guard against the receiver-resolution rewrite). T4: the receiver reached
// through a nested inline-array-element hop (`arr[0][5] := v`), exercising
// `lvalueAddr`'s own recursive IndexExpr branch rather than the plain-local
// `localSlot` branch.
//
// CONSTANTS kept < 256 (runAot's exit code is WEXITSTATUS-masked to 8 bits,
// per the lane protocol) and chosen (seed/index/value all distinct, ADD not
// multiply) so a stale read could never coincidentally match a correct
// write's result.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// T1 — CORE repro: public backing field. _v starts 0; `r[5] := 9` sets
// _v := 9 - 5 = 4; `r[2]` reads _v + 2 = 6. Pre-fix: 2 (the SET silently
// dropped, _v stayed 0). Post-fix: 6.
BEZEL_TEST(indexer_setter_local_record_2502, public_backing_set_then_get) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecLocal2502T1(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecLocal2502T1; "
        "public class C2502T1 "
        "  public static method Main(): int "
        "    var r := new IdxRecLocal2502T1(9); "
        "    r[5] := 9; "
        "    return r[2]; "
        "  end Main; "
        "end C2502T1;"), 6);
}

// T2 — private BACKING field axis. Same arithmetic, different value so this
// case can't coincidentally pass off T1's expected value: `r[3] := 20` sets
// _v := 20 - 3 = 17; `r[4]` reads _v + 4 = 21.
BEZEL_TEST(indexer_setter_local_record_2502, private_backing_set_then_get) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecLocal2502T2(seed: int) "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecLocal2502T2; "
        "public class C2502T2 "
        "  public static method Main(): int "
        "    var r := new IdxRecLocal2502T2(9); "
        "    r[3] := 20; "
        "    return r[4]; "
        "  end Main; "
        "end C2502T2;"), 21);
}

// T3 — CONTROL: a local CLASS's indexer, set-then-get. Classes were never
// affected (a class reference already lowers to a pointer, so the pre-fix
// spill was a correct no-op there) — guards against a regression from the
// receiver-resolution rewrite.
BEZEL_TEST(indexer_setter_local_record_2502, class_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class IdxClsLocal2502T3 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxClsLocal2502T3; "
        "public class C2502T3 "
        "  public static method Main(): int "
        "    using var b := new IdxClsLocal2502T3(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C2502T3;"), 6);
}

// T4 — receiver reached through a nested INLINE-ARRAY-ELEMENT hop
// (`arr[0][5] := v`, §15.1): the indexer's own receiver expression
// (`arr[0]`) resolves through `lvalueAddr`'s recursive IndexExpr branch
// (not the plain-identifier `localSlot` branch T1/T2 exercise), confirming
// the fix's `lvalueAddr` call correctly threads through that recursion too
// rather than only handling a bare local. _v starts 0; `arr[0][5] := 9` sets
// _v := 9 - 5 = 4; `arr[0][2]` reads _v + 2 = 6.
BEZEL_TEST(indexer_setter_local_record_2502, inline_array_element_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecLocal2502T4(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecLocal2502T4; "
        "public class C2502T4 "
        "  public static method Main(): int "
        "    var arr: [1]IdxRecLocal2502T4 := [new IdxRecLocal2502T4(9)]; "
        "    arr[0][5] := 9; "
        "    return arr[0][2]; "
        "  end Main; "
        "end C2502T4;"), 6);
}
