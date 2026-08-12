// Sema — issue #3019 (the `?.` use-site-check twin of #2982/#2964). The DOT
// form (`.`) correctly REJECTS `.Equals(...)` / `.GetHashCode()` on a record /
// choice that is not auto-hashable with "it is not auto-hashable
// (§9.2/§9.3)" / "(§35.7)" (the use-site gate in resolveMemberAccess, #2517).
// The `?.` form COMPILED PAST sema — no diagnostic at any severity — and then
// CRASHED codegen:
//
//   Basic Block in function 'bz$M.C$Main' does not have terminator!
//   label %ncvs.end
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// MEASURED on 332c1b5 (HEAD): `b?.Equals(a)` / `b?.GetHashCode()` on a
// `HasField?` receiver (HasField is a body record with a non-auto field → not
// auto-hashable) compiled with NO sema error and aborted codegen with
// the verifier error above (no binary); the DOT twins `a.Equals(a)` /
// `a.GetHashCode()` on a non-nullable `HasField` correctly drew the §9.2 use-
// site error. The `?.` family commits (c24534c #2990, 66d11f3 #2982, 332c1b5
// #2964) established that `?.` (`NullCondMemberExpr` / a `?.` callee) is a
// DISTINCT AST path that kept missing the `.` form's checks; the §9.2/§35.7
// use-site check is the SAME shape — this is its twin.
//
// ROOT CAUSE. The `.` form's §9.2/§35.7 use-site gate lived in
// resolveMemberAccess (keyed on MemberAccessExpr). The `?.` form parses as
// CallExpr{callee=NullCondMemberExpr} and resolves the callee through
// resolveNullCondMember — a DIFFERENT function that had NO such gate: at its
// `if (!member) return;` point (Equals/GetHashCode carry no MethodSymbol on a
// non-bearing record/choice, so member lookup found nothing) it returned
// silently, and the call travelled to codegen, which crashed. (A class /
// Object / interface receiver is unaffected: it uses the universal §9.6 Object
// surface, where `?.Equals`/`?.GetHashCode` is valid — #2982/#2964.)
//
// THE FIX extracts the gate into a shared helper (checkEqualityMemberUseSite)
// and calls it from BOTH resolveMemberAccess (the `.` form) and
// resolveNullCondMember (the `?.` form), so the two forms enforce the ONE
// check — the one fact the compiler already computes for "is this record /
// choice auto-hashable" (isPositionalRecordEqualityBearing /
// isAutoEqualityBearingBodyRecord / recordHasUserDeclaredEquality /
// isAutoEqualityBearingChoice — the SAME predicates isEqualityBearing, the
// `where T: hashable` §14.2 check, and checkHashContainerKeys use). §7.5: a
// null-conditional access evaluates the member NORMALLY when the receiver is
// non-null, so the member must be AVAILABLE; the gate is on availability, not
// on the receiver's nullness — a `?.Equals` on a non-bearing record is the
// same use-site violation as the `.` form.
//
// SPEC: §7.5 (null-conditional member access — "evaluates to null if the
// receiver is null; otherwise the member is evaluated normally"), §9.2/§9.3
// (record auto-equality + the use-site compile error on `.Equals` /
// `.GetHashCode` for a non-bearing record), §35.7 (the same rule for choices),
// §8.1/§8.3 (Equals/GetHashCode are the universal Object surface, available
// only on auto-hashable records/choices or via user `=`/`<>`).
// v0.240.
//
// Every case drives the REAL `bezelc` AOT path via runAot. For a diagnostic
// fix, "it compiled" / "exit 0" are NOT oracles: without the fix the
// discriminator cases compile PAST sema (no §9.2 error) and CRASH codegen —
// runAot reports that as a Rejected outcome whose diagnostic text is the
// codegen verifier error, NOT the §9.2 message. So each discriminator asserts
// BOTH that the compile was rejected AND that the §9.2/§35.7 message TEXT is
// present (a diagnostic can be right to FIRE and wrong in what it SAYS — assert
// TEXT). The controls assert the message is ABSENT (no false reject on an
// auto-bearing record/choice, and the `.` form's own reject intact).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// runAot returns a JitResult whose `.diagnostics` holds the captured compiler
// stderr on a reject (one Error whose message is the whole stderr, including
// the §9.2/§35.7 line). rejectsAotWith asserts BOTH that the compile did not
// run AND that the needle (the §9.2/§35.7 wording) is in an Error diagnostic.
bool rejectsAotWith(const std::string& program, std::string_view needle) {
    auto r = runAot(program);
    if (r.ran) return false;  // not rejected — ran to completion
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(std::string(needle)) != std::string::npos)
            return true;
    }
    return false;
}

// Control helper: the §9.2/§35.7 needle must be ABSENT from every diagnostic
// (a false reject on an auto-bearing type would put it there). The program may
// still fail for an unrelated reason — this asserts ONLY that the use-site gate
// did not fire, which is the over-rejection guard.
bool noAutoEqError(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("is not auto-hashable") != std::string::npos)
            return false;
    }
    return true;
}

// A body record with a non-auto explicit field → NOT auto-hashable
// (the explicit field is a disqualifier per §9.2, exactly the shape the lit
// suite sema_record_equality__ineligible_equals_call_diagnosed_spec_8_3 uses).
const char* kNonBearingRecord =
    "record HasField public property X: int; "
    "private _cache: int := 0; "
    "public constructor(x: int) X := x; end constructor; end HasField; ";

// An auto-property-only body record → auto-hashable (§9.2: an
// auto-property of an admissible type is participating storage; no
// disqualifier). `.Equals`/`.GetHashCode` are available on it.
const char* kBearingRecord =
    "record Pos public property X: int; "
    "public constructor(x: int) X := x; end constructor; end Pos; ";

// A choice with a class payload → borrow-bearing (§35.4.2) → NOT auto-equality-
// bearing (§35.7).
const char* kNonBearingChoice =
    "class K public constructor() end constructor; end K; "
    "choice Box Some(value: K), None; end Box; ";

// A value-like choice with an admissible (int) payload → auto-hashable
// (§35.7).
const char* kBearingChoice =
    "choice Opt Some(value: int), None; end Opt; ";

std::string wrap(const std::string& prelude, const std::string& body) {
    return "module M3019; " + prelude +
           "class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// Discriminators — FAIL without the fix (no §9.2/§35.7 error; codegen crash),
// PASS with it (sema rejects with the use-site message).
// ===========================================================================

// `?.Equals(...)` on a non-bearing record — the issue's exact case. Without
// the fix: compiles past sema, crashes codegen (IR verifier); the §9.2 message
// is ABSENT, so this test fails. With the fix: rejected at the call site with
// "it is not auto-hashable (§9.2/§9.3)".
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_equals_on_non_bearing_record_rejected_spec_9_2) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(kNonBearingRecord,
             "var a: HasField := new HasField(1); "
             "var b: HasField? := new HasField(1); "
             "_ := b?.Equals(a); "),
        "is not auto-hashable"));
}

// `?.GetHashCode()` on a non-bearing record — the GetHashCode twin of the
// above. §9.2 covers BOTH `.Equals(...)` and `.GetHashCode()`.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_gethashcode_on_non_bearing_record_rejected_spec_9_2) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(kNonBearingRecord,
             "var b: HasField? := new HasField(1); "
             "_ := b?.GetHashCode(); "),
        "is not auto-hashable"));
}

// `?.Equals(...)` on a borrow-bearing choice — §35.7 twin. Without the fix:
// bypasses sema, crashes codegen; with it: rejected with "it is not auto-
// hashable (§35.7) — the choice is borrow-bearing".
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_equals_on_borrow_choice_rejected_spec_35_7) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(kNonBearingChoice,
             "var a: Box := Box.None; "
             "var b: Box? := Box.None; "
             "_ := b?.Equals(a); "),
        "is not auto-hashable"));
}

// `?.GetHashCode()` on a borrow-bearing choice — the GetHashCode twin.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_gethashcode_on_borrow_choice_rejected_spec_35_7) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(kNonBearingChoice,
             "var b: Box? := Box.None; "
             "_ := b?.GetHashCode(); "),
        "is not auto-hashable"));
}

// ===========================================================================
// Controls — PASS both ways (pre-empt). The gate must NOT fire on a type that
// IS auto-hashable (false-reject guard), and the `.` form's own reject
// must remain intact (regression guard for the helper extraction).
// ===========================================================================

// `?.Equals(...)` on an auto-bearing record — no §9.2 use-site error. The
// member IS available, so the null-conditional access is valid (§7.5).
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_equals_on_bearing_record_accepted_control_spec_9_2) {
    BEZEL_EXPECT_TRUE(noAutoEqError(
        wrap(kBearingRecord,
             "var a: Pos := new Pos(1); "
             "var b: Pos? := new Pos(1); "
             "_ := b?.Equals(a); ")));
}

// `?.GetHashCode()` on an auto-bearing record — no §9.2 use-site error.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_gethashcode_on_bearing_record_accepted_control_spec_9_2) {
    BEZEL_EXPECT_TRUE(noAutoEqError(
        wrap(kBearingRecord,
             "var b: Pos? := new Pos(1); "
             "_ := b?.GetHashCode(); ")));
}

// `?.Equals(...)` on a value-like (auto-bearing) choice — no §35.7 error.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_equals_on_bearing_choice_accepted_control_spec_35_7) {
    BEZEL_EXPECT_TRUE(noAutoEqError(
        wrap(kBearingChoice,
             "var a: Opt := Opt.None; "
             "var b: Opt? := Opt.None; "
             "_ := b?.Equals(a); ")));
}

// `?.GetHashCode()` on a value-like (auto-bearing) choice — no §35.7 error.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           nullcond_gethashcode_on_bearing_choice_accepted_control_spec_35_7) {
    BEZEL_EXPECT_TRUE(noAutoEqError(
        wrap(kBearingChoice,
             "var b: Opt? := Opt.None; "
             "_ := b?.GetHashCode(); ")));
}

// The DOT form's own reject — regression guard for the helper extraction. The
// gate was MOVED into checkEqualityMemberUseSite; the `.` form (via
// resolveMemberAccess) must still reject `.Equals(...)` on a non-bearing
// record with the §9.2 message.
BEZEL_TEST(codegen_nullcond_equality_usesite_3019,
           dot_equals_on_non_bearing_record_still_rejected_spec_9_2) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(kNonBearingRecord,
             "var a: HasField := new HasField(1); "
             "_ := a.Equals(a); "),
        "is not auto-hashable"));
}
