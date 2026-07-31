// fix #2507 (§9.2/§23.4, v0.236): two more sites re-implementing indexer
// receiver resolution from scratch, both missing the pointer/aggregate check
// #2502 (this file's `lowerIndexerSet`) and #2498
// (`lowerPropertySetValue`) already fixed for their own sibling call sites —
// `Emitter::lowerIndexerCompound` (`r[i] += v`, LlvmBackend_Assign.cpp) and
// the `IncrementStmt` indexer path (`r[i]++` / `r[i]--`,
// LlvmBackend_Stmts.cpp). Filed by the same lane that fixed #2502/#2498,
// while sweeping the defect shape elsewhere per the lane protocol.
//
// Root cause (both sites, identically): `llvm::Value* obj =
// lowerExpr(*ix->target);` LOADS a byte-for-byte COPY for a named
// record-typed receiver (records are by-value, §9.2), then binds that raw
// struct value directly as `currentThis_` for the inline expression-getter
// and setter calls with NO pointer/aggregate check at all. A block-form
// getter's own local `objPtr` spill (present in both sites) does check —
// but that spill was never written back to the shared `obj`, so the
// subsequent setter call (and, in `lowerIndexerCompound`, the
// expression-getter call too) still bound the unchecked raw copy. Confirmed
// on origin/main by direct probe (this file's own T1/T5 repros): both crash
// IR verification —
//   GEP base pointer is not a vector or a vector of pointers
//     %10 = getelementptr inbounds nuw %record.IdxRecCompound2507T1, ...
//   Store operand must be a pointer.
//     store i32 %25, %record.IdxRecCompound2507T1 %21, align 4
//   bezel codegen: IR verification failed (malformed IR emitted; ...)
// — a compile-time abort on a legitimate, spec-permitted shape (§23.4
// read-write indexer on a record; compound-assign/increment forms apply
// uniformly to any lvalue-shaped target per §6.6/§20).
//
// Fix mirrors #2502/#2498 exactly: prefer `lvalueAddr(*ix->target, elemTy)`
// (the receiver's OWN storage address) over `lowerExpr` (a copy); fall back
// to `lowerExpr` + spill only when the receiver expression is genuinely NOT
// an addressable place (a fresh temporary). No `isEnumReceiver` guard is
// needed here (unlike #2498's property-setter fix) — §23.5 "Enums may not
// declare indexers" is enforced by `checkEnumRestrictions` at the MEMBER
// level (independent of body form), so an enum receiver can never reach
// either function at all; T9 below pins that rejection directly.
//
// On DELEGATION (raised explicitly by the issue text, "consider whether the
// right fix is to delegate rather than patch a third/fourth copy"): both
// sites were deliberately NOT rewritten to call `lowerIndexerGet` +
// `lowerIndexerSet` directly. Both of those functions independently
// re-evaluate `ix.target` and each index expression via their own call to
// `lowerExpr`/`lvalueAddr`; calling them back-to-back for a
// read-modify-write would evaluate the receiver and index expressions
// TWICE, violating §23.4's explicit single-evaluation contract ("The
// receiver and each index expression are evaluated ONCE and reused for
// both calls, avoiding observable double side-effects" — this file's own
// `lowerIndexerCompound` header comment, predating this fix). Delegating
// safely would require restructuring `lowerIndexerGet`/`lowerIndexerSet` to
// accept a pre-lowered receiver/index, which is a larger change to
// already-fixed, already-tested functions than this fix attempts. The
// duplication BETWEEN `lowerIndexerCompound` and the `IncrementStmt`
// indexer path (they are near-identical get-then-set flows, one general OP,
// one always ±1) is a separate, smaller opportunity — filed as #2510 rather
// than attempted here, to keep this commit to the proven, minimal,
// mechanically-verified fix.
//
// T1/T2: `r[i] += v` on a local record, public/private backing (distinct
// constants so neither could coincidentally pass). T3/T4: `r[i]++` /
// `r[i]--`. T5/T6: local CLASS control for compound/increment (classes were
// never affected — a class reference already lowers to a pointer). T7/T8:
// receiver reached through a nested inline-array-element hop (`arr[0][i]`,
// §15.1), exercising `lvalueAddr`'s recursive IndexExpr branch. T9: enum
// control — §23.5/§10.5 rejects an indexer declaration on an enum outright,
// confirming this fix's codegen paths are unreachable for an enum receiver
// (no defensive guard needed, unlike #2498's property-setter sibling).
//
// CONSTANTS kept < 256 (runAot's exit code is WEXITSTATUS-masked to 8 bits,
// per the lane protocol) and ADDITION only (never multiplication, per the
// same guidance) so a stale/crashed read (-999) could never coincidentally
// match a correct write's result.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// T1 — CORE repro: `r[i] += v` on a local record, public backing. _v starts
// 0; `r[5] += 3`: get r[5] = _v+5 = 5, new value 5+3=8, set _v := 8-5 = 3;
// `r[2]` reads _v+2 = 5. Pre-fix: IR verification failure -> run() = -999.
BEZEL_TEST(indexer_compound_increment_local_2507, compound_add_public_backing) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecCompound2507T1(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecCompound2507T1; "
        "public class C2507T1 "
        "  public static method Main(): int "
        "    var r := new IdxRecCompound2507T1(9); "
        "    r[5] += 3; "
        "    return r[2]; "
        "  end Main; "
        "end C2507T1;"), 5);
}

// T2 — private BACKING field axis, distinct constants: `r[3] += 20`: get
// r[3] = 0+3=3, new value 3+20=23, set _v := 23-3 = 20; `r[4]` reads
// _v+4 = 24.
BEZEL_TEST(indexer_compound_increment_local_2507, compound_add_private_backing) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecCompound2507T2(seed: int) "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecCompound2507T2; "
        "public class C2507T2 "
        "  public static method Main(): int "
        "    var r := new IdxRecCompound2507T2(9); "
        "    r[3] += 20; "
        "    return r[4]; "
        "  end Main; "
        "end C2507T2;"), 24);
}

// T3 — `r[i]++` on a local record. r[5] = 0+5=5, new value 5+1=6,
// set _v := 6-5 = 1; r[2] reads _v+2 = 3.
BEZEL_TEST(indexer_compound_increment_local_2507, increment_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecIncr2507T3(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecIncr2507T3; "
        "public class C2507T3 "
        "  public static method Main(): int "
        "    var r := new IdxRecIncr2507T3(9); "
        "    r[5]++; "
        "    return r[2]; "
        "  end Main; "
        "end C2507T3;"), 3);
}

// T4 — `r[i]--` on a local record, distinct constants. r[3] = 0+3=3,
// new value 3-1=2, set _v := 2-3 = -1; r[7] reads _v+7 = 6.
BEZEL_TEST(indexer_compound_increment_local_2507, decrement_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecDecr2507T4(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecDecr2507T4; "
        "public class C2507T4 "
        "  public static method Main(): int "
        "    var r := new IdxRecDecr2507T4(9); "
        "    r[3]--; "
        "    return r[7]; "
        "  end Main; "
        "end C2507T4;"), 6);
}

// T5 — CONTROL: local CLASS compound-assign (unaffected — a class reference
// already lowers to a pointer). Same arithmetic as T1: expect 5.
BEZEL_TEST(indexer_compound_increment_local_2507, class_control_compound_add) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class IdxClsCompound2507T5 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxClsCompound2507T5; "
        "public class C2507T5 "
        "  public static method Main(): int "
        "    using var c := new IdxClsCompound2507T5(); "
        "    c[5] += 3; "
        "    return c[2]; "
        "  end Main; "
        "end C2507T5;"), 5);
}

// T6 — CONTROL: local CLASS increment (unaffected). Same arithmetic as T3:
// expect 3.
BEZEL_TEST(indexer_compound_increment_local_2507, class_control_increment) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class IdxClsIncr2507T6 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxClsIncr2507T6; "
        "public class C2507T6 "
        "  public static method Main(): int "
        "    using var c := new IdxClsIncr2507T6(); "
        "    c[5]++; "
        "    return c[2]; "
        "  end Main; "
        "end C2507T6;"), 3);
}

// T7 — receiver reached through a nested INLINE-ARRAY-ELEMENT hop
// (`arr[0][5] += v`, §15.1): the indexer's own receiver expression
// (`arr[0]`) resolves through `lvalueAddr`'s recursive IndexExpr branch,
// not the plain-identifier `localSlot` branch T1-T4 exercise. Same
// arithmetic as T1: expect 5.
BEZEL_TEST(indexer_compound_increment_local_2507, array_element_receiver_compound) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecArrCompound2507T7(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecArrCompound2507T7; "
        "public class C2507T7 "
        "  public static method Main(): int "
        "    var arr: [1]IdxRecArrCompound2507T7 := [new IdxRecArrCompound2507T7(9)]; "
        "    arr[0][5] += 3; "
        "    return arr[0][2]; "
        "  end Main; "
        "end C2507T7;"), 5);
}

// T8 — same nested-array-element receiver axis, for `++`. Same arithmetic
// as T3: expect 3.
BEZEL_TEST(indexer_compound_increment_local_2507, array_element_receiver_increment) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record IdxRecArrIncr2507T8(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRecArrIncr2507T8; "
        "public class C2507T8 "
        "  public static method Main(): int "
        "    var arr: [1]IdxRecArrIncr2507T8 := [new IdxRecArrIncr2507T8(9)]; "
        "    arr[0][5]++; "
        "    return arr[0][2]; "
        "  end Main; "
        "end C2507T8;"), 3);
}

// T9 — ENUM control: §23.5 forbids indexers on an enum outright ("Enums may
// not declare indexers"), enforced by `checkEnumRestrictions`
// (TypeResolution_Checks.cpp) at the MEMBER level — independent of body
// form, so this fix's codegen paths can never be reached by an enum
// receiver (unlike #2498's property-setter sibling, which needed an
// explicit `isEnumReceiver` guard against a separate, then-open sema gap,
// #2499). Assert on diagnostic TEXT, not the coarse "did not run": runAot's
// WIFSIGNALED path synthesizes an Error for a runtime SIGSEGV too, so a
// bare failure check cannot tell a genuine rejection from a crash.
BEZEL_TEST(indexer_compound_increment_local_2507, enum_indexer_rejected_10_5) {
    auto r = bezel::test::runAot(
        "module M; "
        "public enum EnumIdx2507T9 "
        "  Red, Green, Blue; "
        "  public property [i: int]: int "
        "    get => i; "
        "  end; "
        "end EnumIdx2507T9; "
        "public class C2507T9 "
        "  public static method Main(): int "
        "    var e := EnumIdx2507T9.Red; "
        "    return e[3]; "
        "  end Main; "
        "end C2507T9;");
    bool sawIndexerDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find("may not declare an indexer") != std::string::npos) {
            sawIndexerDiag = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(sawIndexerDiag);
}
