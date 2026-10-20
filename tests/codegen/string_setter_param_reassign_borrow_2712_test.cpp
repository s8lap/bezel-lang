// Codegen / refcounting — the #2696 defect at the INLINED-ACCESSOR binding
// sites. Issue #2712 (filed for the inlined indexer setter), plus the `value :=`
// half folded in per the issue's own scope correction, plus the inlined RECORD
// constructor recorded on #2707.
//
// Spec: §28.1 "Where the `owned` modifier can appear" — "On any other type — a
// primitive, `string`, a function type, an ordinary record, an enum, a
// non-owning inline array, or the unit type — `owned` is a compile error: such
// values carry no disposal obligation to transfer (§28.9)" — so a `string`
// parameter is ALWAYS an unmodified borrow whose one reference belongs to the
// CALLER. §23.3 "Block-form properties" says the same for the implicit setter
// parameter: "The setter's implicit `value` parameter has no modifier by default
// and takes a borrow of the property's declared type." Also §23.4 "Indexers",
// §28.9 "Runtime-managed types", §4.6 "`string`". Spec revision v0.240.
//
// THE DEFECT. `lowerStringLocalReassign` (`s := e`) and the §4.6 `s += e` path
// both emit a drop-on-overwrite `bz_string_release` of the value they displace,
// and both admit a PARAMETER target (their gate is localSlot(), which resolves a
// ParameterSymbol through paramSlots_ AND a LocalSymbol through locals_). That
// release is correct for a LOCAL, whose slot was given a +1 at bind; a
// PARAMETER's slot was given nothing. #2696 fixed the two bodies lowered by
// `emitFunctionBody` and by `inlineCtorBody`. Every INLINED ACCESSOR binding
// site had the same hole: it rebuilds paramSlots_/locals_ with fresh entry
// allocas and lowers the accessor body into the ENCLOSING function WITHOUT
// touching `currentFn_`, so when that enclosing body is the analyzed one
// `currentFn_ == stringReleaseEligibleFn_` still holds, the release fires, and
// nothing retained: the caller's borrow is freed.
//
// ⚠ THE FALSE NEGATIVE THIS FILE IS BUILT TO AVOID. The temporary form
// `t[$"v-{i}"] := i;` does NOT fault — nothing reads the string afterwards, so
// the premature free is simply unreachable. Only binding the index/value to
// something that is READ AFTER THE ASSIGNMENT turns the latent free into an
// observable use-after-free. Every case below therefore passes the string as a
// method parameter (or a local) and reads it back after the accessor call.
//
// ⚠ Every string is built at RUNTIME (`$"v-{i}"`): `"ab" + "cd"` constant-folds
// to an rc sentinel on which `bz_string_release` no-ops — the trap that has made
// probes in this family vacuous repeatedly.
//
// MECHANICAL FAIL -> PASS, both oracles, all seven defect cases (A-G):
//   * plain (unsanitized) AOT link+run : exit 134 (SIGABRT — glibc heap
//     corruption on the caller's later free) -> exit 71;
//   * ASan/LSan                        : `heap-use-after-free`, frames
//     `bz_string_release <- main` (or `<- bz$M.C$Use`) -> clean, exit 71.
//
// ⚠ RESIDUAL, BY DESIGN (inherited from #2696): no scope-end release is
// registered for the retained slot, so the LAST value assigned to it leaks
// unless that value is a literal/sentinel. Every case below therefore ends its
// assignment chain on a literal, so all of them run leak-free under LSan and the
// sanitizer is a live oracle in BOTH directions — an over-correction (a retain
// where the release is gated off) shows up as a leak, not as silence. Case (H)
// is the dedicated over-correction guard.
//
// ⚠ NOT COVERED HERE, AND WHY: passing a `string` LOCAL directly as an indexer
// index argument (`var s := $"v-{i}"; t[s] := i;`) leaks that string once per
// call — the local's scope-end release is suppressed and no release fires for
// the borrow. That leak is PRE-EXISTING and independent of this fix: a setter
// body that assigns to nothing at all leaks the identical 731 bytes / 40
// allocations on both sides of this change. Filed as #2717; the cases below
// route the string through a method parameter so LSan stays a clean oracle for
// the defect this file is about.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// Every program below returns sum over i in 1..20 of len("v-{i}")
//   = 9*3 + 11*4 = 71.
constexpr int kExpected = 71;

// (A) THE ISSUE'S OWN CASE — an inlined INDEXER SETTER that reassigns its own
// `string` INDEX parameter (`lowerIndexerSet`, LlvmBackend_Assign.cpp). `Use`
// reads `s.Length` after the assignment, which is what makes the premature free
// observable. The trailing `k := "zz"` reclaims the concat the frame now owns,
// so the program is fully balanced and LSan fails it in BOTH directions:
// pre-fix on the use-after-free; and had the bug been "fixed" by deleting the
// drop-on-overwrite release instead of adding the missing retain, on the leaked
// concat.
const char* kInlinedIndexerSetterReassignsIndexParam =
    "module M; class Table "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => N; "
    "    set => do "
    "      k := k + \"-x\"; "          // frees the CALLER's string, pre-fix
    "      N := N + k.Length; "
    "      k := \"zz\"; "              // reclaims the frame-owned concat
    "    end; "
    "  end; "
    "end Table; "
    "class C "
    "  public static method Use(t: Table, s: string): int "
    "    t[s] := 1; "
    "    return s.Length; "            // reads a string the setter freed
    "  end Use; "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Use(t, $\"v-{i}\"); "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (B) THE `value :=` HALF, folded in here per #2712's scope correction. The
// implicit setter parameter is NOT in `ps->indexerParams`: sema anchors its
// LocalSymbol on the PropertyDecl and codegen injects it straight into locals_,
// so it bypasses both `bindParameters` and `armStringLocalRefcount` and had no
// bind-time +1 either. §23.3 gives it the same borrow semantics as a by-value
// parameter, so it needs the same retain.
const char* kInlinedIndexerSetterReassignsValue =
    "module M; class Table "
    "  public N: int; "
    "  public property [k: int]: string "
    "    get => \"q\"; "
    "    set => do "
    "      N := N + value.Length; "
    "      value := \"zz\"; "          // frees the CALLER's string, pre-fix
    "      N := N + value.Length; "
    "    end; "
    "  end; "
    "end Table; "
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      t[i] := s; "
    "      total := total + s.Length; "  // reads a string the setter freed
    "    end; "
    "    return total; "                 // 71
    "  end Main; "
    "end C;";

// (C) The §4.6 COMPOUND path — `lowerIndexerCompound` desugars `t[s] += 1` to a
// get, an op, and a SECOND inlined setter binding, which had its own copy of the
// hole.
const char* kInlinedIndexerCompoundReassignsIndexParam =
    "module M; class Table "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => N; "
    "    set => do "
    "      k := k + \"-x\"; "
    "      N := value + k.Length; "
    "      k := \"zz\"; "
    "    end; "
    "  end; "
    "end Table; "
    "class C "
    "  public static method Use(t: Table, s: string): int "
    "    t[s] += 1; "
    "    return s.Length; "
    "  end Use; "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Use(t, $\"v-{i}\"); "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (D) The `++`/`--` path (LlvmBackend_Stmts.cpp's IncrementStmt indexer branch)
// — a third, independent inlined-setter binding loop with the same hole.
const char* kInlinedIndexerIncrementReassignsIndexParam =
    "module M; class Table "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => N; "
    "    set => do "
    "      k := k + \"-x\"; "
    "      N := value + k.Length; "
    "      k := \"zz\"; "
    "    end; "
    "  end; "
    "end Table; "
    "class C "
    "  public static method Use(t: Table, s: string): int "
    "    t[s]++; "
    "    return s.Length; "
    "  end Use; "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Use(t, $\"v-{i}\"); "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (E) A NON-INDEXER instance property's inlined block setter
// (`lowerPropertySetValue`). Not listed on the issue — found by sweeping the
// shape — and live: `value := "zz"` freed the caller's string.
const char* kInlinedInstancePropertySetterReassignsValue =
    "module M; class Holder "
    "  public N: int; "
    "  public property P: string "
    "    get => \"q\"; "
    "    set => do "
    "      N := N + value.Length; "
    "      value := \"zz\"; "
    "      N := N + value.Length; "
    "    end; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      h.P := s; "
    "      total := total + s.Length; "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (F) The STATIC-property branch of the same function — a separate binding site
// a few dozen lines above (E)'s, equally live.
const char* kInlinedStaticPropertySetterReassignsValue =
    "module M; class Holder "
    "  public static N: int; "
    "  public static property P: string "
    "    get => \"q\"; "
    "    set => do "
    "      Holder.N := Holder.N + value.Length; "
    "      value := \"zz\"; "
    "      Holder.N := Holder.N + value.Length; "
    "    end; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      Holder.P := s; "
    "      total := total + s.Length; "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (G) The INLINED RECORD CONSTRUCTOR (`buildRecordValue`, LlvmBackend_Exprs2.cpp)
// — the third member of the family, recorded on #2707. Identical shape to
// `inlineCtorBody` (which #2696 fixed): it binds `cs->params` into paramSlots_
// and lowers `cd->body` with `currentFn_` unchanged.
const char* kInlinedRecordCtorReassignsStringParam =
    "module M; "
    "record Rec "
    "  public n: int; "
    "  public constructor(s: string) "
    "    s := s + \"-x\"; "
    "    this.n := s.Length; "
    "    s := \"zz\"; "
    "  end constructor; "
    "end Rec; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var str := $\"v-{i}\"; "
    "      var r := new Rec(str); "
    "      total := total + str.Length; "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (H) OVER-CORRECTION GUARD — a setter that assigns to NEITHER an index
// parameter NOR `value` must take NO retain, or the caller's string leaks once
// per call. Green before and after by design; it fails here if the body walk
// that gates the new retain ever admits a non-assigning body. (The co-gate
// `currentFn_ != stringReleaseEligibleFn_ -> return` is guarded by #2696's own
// cases (E)/(F), which pin the same predicate at the emitFunctionBody level.)
const char* kSetterNeverAssignsTakesNoRetain =
    "module M; class Holder "
    "  public N: int; "
    "  public property P: string "
    "    get => \"q\"; "
    "    set => do "
    "      N := N + value.Length; "
    "    end; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      h.P := s; "
    "      total := total + s.Length; "
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2712 — an inlined indexer setter that reassigns its own `string` index
// parameter no longer frees the caller's borrow. §28.1 / §23.4 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_indexer_setter_reassigning_index_param_spec_23_4) {
    JitResult r = runAotChecked(kInlinedIndexerSetterReassignsIndexParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // no use-after-free AND no leak
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// The same program on the PLAIN, unsanitized path: pre-fix it aborts (exit 134,
// glibc heap corruption); the value is only readable back when the string is
// still alive. Pins the defect independently of any sanitizer.
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_indexer_setter_unsanitized_value_survives_spec_23_4) {
    JitResult r = runAot(kInlinedIndexerSetterReassignsIndexParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (B) The implicit `value` parameter is a borrow too (§23.3) — assigning to it
// must not release the caller's string.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_indexer_setter_reassigning_value_spec_23_3) {
    JitResult r = runAotChecked(kInlinedIndexerSetterReassignsValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (C) The §4.6 compound path `t[s] += 1` — a second inlined setter binding.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_indexer_compound_setter_reassigning_index_param_spec_4_6) {
    JitResult r = runAotChecked(kInlinedIndexerCompoundReassignsIndexParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (D) The `++` path — a third inlined setter binding.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_indexer_increment_setter_reassigning_index_param_spec_23_4) {
    JitResult r = runAotChecked(kInlinedIndexerIncrementReassignsIndexParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (E) A non-indexer INSTANCE property's inlined block setter. §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_instance_property_setter_reassigning_value_spec_23_3) {
    JitResult r = runAotChecked(kInlinedInstancePropertySetterReassignsValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (F) The STATIC-property branch of the same path. §23.3 / §22.12.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_static_property_setter_reassigning_value_spec_23_3) {
    JitResult r = runAotChecked(kInlinedStaticPropertySetterReassignsValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (G) The inlined RECORD constructor — the family's third binding site
// (#2707). §28.1 / §9.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           inlined_record_ctor_reassigning_string_param_spec_28_1) {
    JitResult r = runAotChecked(kInlinedRecordCtorReassignsStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (H) Over-correction guard: a setter that assigns to nothing takes no retain,
// so the caller's string does not leak. §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_setter_param_reassign_2712,
           setter_that_never_assigns_takes_no_retain_spec_28_9) {
    JitResult r = runAotChecked(kSetterNeverAssignsTakesNoRetain);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // a spurious retain would leak here
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}
