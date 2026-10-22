// Codegen / refcounting — the #2720 defect at the INLINED RECORD CONSTRUCTOR
// (`buildRecordValue`, src/codegen/LlvmBackend_Exprs2.cpp:7576), filed as the
// third member of the #2696 family (the first two are `inlineCtorBody` #2696
// and the inlined indexer/property setters #2712). Folded into commit `2b9ab82`
// alongside #2712; filed as its own number so the record-constructor site is
// discoverable independently of an issue whose subject describes inlined
// ACCESSORS.
//
// Spec: §28.3 "Local variables, ownership, and borrows" — the parameter slot
// of a by-value `string` parameter carries no bind-time +1 (§28.1 makes
// `owned` over `string` a compile error; §23.3 / §28.9 confirms `string`
// parameters and setter `value` are borrows whose single reference belongs to
// the CALLER). §28.9 "Runtime-managed types" — the only authority on the
// drop-on-overwrite `bz_string_release` that fires when a parameter is
// reassigned inside a body analyzed by the enclosing function's own
// `collectStringReleaseEligible` walk.
//
// THE DEFECT. `buildRecordValue` binds the record ctor's parameters into
// `paramSlots_` (with fresh entry allocas) and lowers `cd->body` with
// `currentFn_` UNCHANGED. So the SAME shape #2696 fixed in `inlineCtorBody`
// applies here: when the enclosing function is the one whose string analysis
// ran (`currentFn_ == stringReleaseEligibleFn_` still holds), the drop-on-
// overwrite release on `s := e` (or `s := s + "-x"`) admits the
// `ParameterSymbol`, fires, decrements the caller's single refcount, frees
// the string, and the caller reads a dangling pointer.
//
// THE FIX (already in `2b9ab82`, line 8053 of LlvmBackend_Exprs2.cpp). One
// call to `retainAssignedStringParams(cd->body, cs->params)` at the start of
// the inlined-ctor binding prologue. The helper opens with `if (currentFn_
// != stringReleaseEligibleFn_) return;`, so the retain and the release it
// pairs cannot desynchronize: every body whose drop-on-overwrite release is
// inert (a destructor body, an expression-bodied lambda, a `finally` replayed
// by an iterator's Dispose) automatically skips the retain — and every body
// whose release is active takes the matching +1. No call-site audit needed.
//
// ⚠ THE FALSE NEGATIVE THIS FILE IS BUILT TO AVOID. A probe that constructs
// a record but never reads the string back does not fault — the premature
// free is unreachable. Every case below passes the string as a ctor argument
// and reads it back after the construction call. Likewise every constant is
// built at RUNTIME (`$"v-{i}"`); a folded constant (`"ab" + "cd"`) is an RC
// sentinel on which `bz_string_release` no-ops, the trap that has made probes
// in this family vacuous repeatedly.
//
// MECHANICAL FAIL -> PASS, both oracles, every defect case (A-D):
//   * plain (unsanitized) AOT link+run : exit 134 (SIGABRT — glibc heap
//     corruption on the caller's later free) -> exit 71;
//   * ASan/LSan                        : `heap-use-after-free`, frame
//     `bz_string_release <- bz$M.C$Main` -> clean, exit 71.
//
// ⚠ RESIDUAL, BY DESIGN (inherited from #2696): no scope-end release is
// registered for the retained slot, so the LAST value assigned to it leaks
// unless that value is a literal/sentinel. Every case below therefore ends
// its assignment chain on a literal (`s := "zz"`), so all of them run
// leak-free under LSan and the sanitizer is a live oracle in BOTH
// directions — an over-correction (a retain where the release is gated off)
// shows up as a leak, not as silence. Case (E) is the dedicated
// over-correction guard.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every program below returns sum over i in 1..20 of len("v-{i}")
//   = 9*3 + 11*4 = 71.
constexpr int kExpected = 71;

// (A) THE ISSUE'S OWN CASE — an inlined RECORD CONSTRUCTOR that reassigns
// its own `string` parameter. `Main` reads `str.Length` AFTER `new Rec(str)`,
// which is what makes the premature free observable. The trailing `s := "zz"`
// reclaims the concat the frame now owns, so the program is fully balanced
// and LSan fails it in BOTH directions: pre-fix on the use-after-free; and
// had the bug been "fixed" by deleting the drop-on-overwrite release instead
// of adding the missing retain, on the leaked concat.
//
// Shape mirrors case (G) of tests/codegen/string_setter_param_reassign_
// borrow_2712_test.cpp but is its OWN regression file so the record-ctor
// site is pinned even if the 2712 file is ever split.
const char* kInlinedRecordCtorReassignsStringParam =
    "module M; "
    "record Rec "
    "  public n: int; "
    "  public constructor(s: string) "
    "    s := s + \"-x\"; "          // frees the CALLER's string, pre-fix
    "    this.n := s.Length; "
    "    s := \"zz\"; "              // reclaims the frame-owned concat
    "  end constructor; "
    "end Rec; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var str := $\"v-{i}\"; "
    "      var r := new Rec(str); "
    "      total := total + str.Length; "   // reads a string the ctor freed
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (B) THE FIELD-ASSIGN SHAPE — a record with a `string` FIELD whose
// constructor assigns the parameter straight into the field (`this.s := p`),
// then reassigns `p` later (`p := "zz"`). Pre-fix the `p := "zz"` line
// releases the borrow the field is now holding; reading `r.s.Length` after
// the construction dereferences the freed string. Same defect class as (A),
// but the surviving alias lives in the RECORD'S field, not the caller's
// local — proving the fix at the ctor call site, not at the caller's read.
//
// The trailing `p := "zz"` is required so the retained string isn't leaked
// (LSan must stay silent). The field itself takes no retain at dispose time
// (§9.2: a record field is copy-semantics storage; no class-side dispose
// body runs), so the math is: caller +1 → ctor prologue retain +1 → field
// +1 (this.s := p) → drop-on-overwrite -1 = +2 (caller + retain + field).
// Caller drops -> +1; record drops its field -> 0; retained literal "zz"
// outlives the function and is dropped at process exit.
const char* kInlinedRecordCtorAssignsParamToStringField =
    "module M; "
    "record Rec "
    "  public s: string; "
    "  public n: int; "
    "  public constructor(p: string) "
    "    this.s := p; "              // field borrows caller's string
    "    this.n := p.Length; "
    "    p := \"zz\"; "              // pre-fix: drops the field's borrow too
    "  end constructor; "
    "end Rec; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var str := $\"v-{i}\"; "
    "      var r := new Rec(str); "
    "      total := total + r.s.Length; "  // reads a string the ctor freed
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (C) THE COMPOUND CASE — both shapes in one body: a field store AFTER a
// reassign, exercising the same `retainAssignedStringParams` site on a
// field-targeting + plain-reassign pair. The field is BORROWED from the
// caller's original string (the field store happens before any param
// reassign that would invalidate it), then the param is reassigned. The
// caller reads `str.Length` afterwards — the original-borrow path the fix
// protects. Distinct from (A) because the body performs a field store, not
// just a string concat.
const char* kInlinedRecordCtorReassignsAndAssignsToField =
    "module M; "
    "record Rec "
    "  public s: string; "
    "  public n: int; "
    "  public constructor(p: string) "
    "    this.s := p; "              // field borrows caller's string
    "    p := p + \"-y\"; "          // reassign: pre-fix frees the borrow
    "    this.n := p.Length; "
    "    p := \"zz\"; "              // drop the frame-owned concat
    "  end constructor; "
    "end Rec; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var str := $\"v-{i}\"; "
    "      var r := new Rec(str); "
    "      total := total + str.Length; "  // reads a string the ctor freed
    "    end; "
    "    return total; "               // 71
    "  end Main; "
    "end C;";

// (D) THE §4.6 COMPOUND PATH — `p := p + "..."` is a single AssignmentStmt
// node that uses the same drop-on-overwrite release as the bare `:=`. The
// helper sees AssignmentStmt op == "concat" as a `:=` candidate and retains
// the same way, but it is worth pinning separately: the compound-shape path
// is what several other call sites (e.g. lowerIndexerCompound) test, and a
// regression that admitted one form and not the other would slip past a
// single-shape test.
const char* kInlinedRecordCtorCompoundConcatOnStringParam =
    "module M; "
    "record Rec "
    "  public n: int; "
    "  public constructor(s: string) "
    "    s := s + \"-x\"; "          // concat-and-reassign
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

// (E) OVER-CORRECTION GUARD — a record ctor that assigns to NEITHER its own
// parameter NOR the field (just initializes a field with a literal) must
// take NO retain, or the param's bind-time +0 would pair with a +1 retain
// and leak the borrow once per call. Green before and after by design; it
// fails here if the body walk that gates the new retain ever admits a
// body that does not actually reassign.
const char* kInlinedRecordCtorNeverAssignsTakesNoRetain =
    "module M; "
    "record Rec "
    "  public n: int; "
    "  public constructor(s: string) "
    "    this.n := s.Length; "       // read s, never write it
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

}  // namespace

namespace {
// Print any ASan / harness diagnostic message so a FAIL captures the headline
// in the test log (the gtest default surfaces only the BEZEL_EXPECT_* line,
// which is not enough to prove the fix from a CI run). No-op on success.
void maybeDumpDiagnostics(const char* testName, const JitResult& r) {
    if (r.ran && r.exitCode == 71) return;
    std::fprintf(stderr, "\n===== [%s] diagnostics dump (ran=%d exit=%d) =====\n",
                 testName, r.ran, r.exitCode);
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::fprintf(stderr, "  [%s] %s\n",
                     d.severity == bezel::Severity::Error ? "ERROR" :
                     d.severity == bezel::Severity::Warning ? "WARN" : "NOTE",
                     d.message.c_str());
    }
    std::fprintf(stderr, "===== [%s] end diagnostics =====\n", testName);
    std::fflush(stderr);
}
}  // namespace

// =====================================================================
// (A) #2720 — an inlined record constructor that reassigns its own `string`
// parameter no longer frees the caller's borrow. §28.3 / §9.
// =====================================================================
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_reassigning_string_param_spec_28_3) {
    JitResult r = runAotChecked(kInlinedRecordCtorReassignsStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));   // ASan toolchain unavailable fails the row (#3733)
    maybeDumpDiagnostics("reassigning_string_param_spec_28_3", r);
    BEZEL_EXPECT_TRUE(r.ran);          // no use-after-free AND no leak
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// The same program on the PLAIN, unsanitized path: pre-fix it aborts (exit 134,
// glibc heap corruption); the value is only readable back when the string is
// still alive. Pins the defect independently of any sanitizer.
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_unsanitized_value_survives_spec_28_3) {
    JitResult r = runAot(kInlinedRecordCtorReassignsStringParam);
    maybeDumpDiagnostics("unsanitized_value_survives_spec_28_3", r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (B) The field-assign shape — the caller's borrow ends up in the record's
// field. The fix at the ctor call site protects the field's alias. §28.3 /
// §9.
// =====================================================================
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_assigns_param_to_string_field_spec_28_3) {
    JitResult r = runAotChecked(kInlinedRecordCtorAssignsParamToStringField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    maybeDumpDiagnostics("assigns_param_to_string_field_spec_28_3", r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (C) Compound case — field-assign AND reassign in the same body.
// =====================================================================
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_reassigns_and_assigns_to_field_spec_28_3) {
    JitResult r = runAotChecked(kInlinedRecordCtorReassignsAndAssignsToField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    maybeDumpDiagnostics("reassigns_and_assigns_to_field_spec_28_3", r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (D) The §4.6 compound-concat shape on the ctor's string parameter.
// =====================================================================
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_compound_concat_on_string_param_spec_4_6) {
    JitResult r = runAotChecked(kInlinedRecordCtorCompoundConcatOnStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    maybeDumpDiagnostics("compound_concat_on_string_param_spec_4_6", r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}

// =====================================================================
// (E) Over-correction guard: a ctor that assigns to nothing takes no retain,
// so the param's borrow doesn't leak. §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_build_record_value_caller_borrow_2720,
           inlined_record_ctor_never_assigns_takes_no_retain_spec_28_9) {
    JitResult r = runAotChecked(kInlinedRecordCtorNeverAssignsTakesNoRetain);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));   // a spurious retain would leak here
    maybeDumpDiagnostics("never_assigns_takes_no_retain_spec_28_9", r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpected);
}