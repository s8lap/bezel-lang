// Codegen — §35.7 (v0.210, #2083 REWORK): choice equality/hash must be REAL
// CONTROL FLOW (switch on tag → one block per payload-bearing case → PHI
// join), not a straight-line select chain. Choice construction fills ONLY the
// tag and the active case's payload slot; every inactive-case payload slot is
// `undef` (LlvmBackend_Calls2.cpp). A select chain executes every case's
// payload compare/hash unconditionally and only masks the RESULT — so for any
// string / record-with-string / nested-choice payload case, the pre-rework
// emitters ran emitStringEq / the string-hash loop through an UNDEF POINTER:
//   - emitChoiceEquality: same-tag compare of two values in a payload-free
//     case still executed the string case's compare on both undef slots;
//   - emitChoiceHashCode: NO tag precondition at all — GetHashCode on a
//     payload-free active case of a string-bearing choice deterministically
//     walked the string-hash loop through undef (the pin tests below).
// The lane's original tests used int payloads only, which a select chain
// masks soundly — these tests pin the pointer-payload class the review found.
//
// Spec authority: §35.7 (equality/hash over the ACTIVE case, v0.210), §9.2 /
// §9.3 (choice components in auto-equality records), §3.6 / §8.3 (hash-
// container key contract; equal values must hash equal). Also pins finding 2:
// the DIRECT bare-choice operator surface (`a = b` on choice-typed operands,
// sema-legal for value-like choices per the §35.7 use-site rule) and the
// direct `.Equals()` / `.GetHashCode()` value-semantics surface.
//
// ALL tests run via runAotChecked (ASan/LSan): the undef-pointer walk is a
// wild read ASan reports (or a plain SEGV), flipping r.ran to false.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 10 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 10 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

}  // namespace

// ─── THE PIN (finding 1, hash half): GetHashCode with a payload-free ACTIVE
// case of a string-bearing choice. Pre-rework emitChoiceHashCode had no tag
// precondition: it unconditionally folded Note's string payload — for a
// Wrap(Msg.Empty) value that slot is undef → the string-hash loop walks a
// wild pointer. Deterministic pre-fix crash; post-rework the Note arm is
// emitted under `switch tag` and never executes for Empty.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_record_choice_field_payload_free_gethashcode_pin) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "record Wrap(M: Msg); "
        "class C public static method Main(): int "
        "  var w := new Wrap(Msg.Empty); "
        "  var x := new Wrap(Msg.Empty); "
        "  return (if w.GetHashCode() = x.GetHashCode() then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // pre-rework: undef-pointer string hash → crash
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // equal values hash equal (tag-only fold)
}

// ─── THE PIN (finding 1, equality half): same-tag compare in a payload-free
// case of a string-bearing choice. Pre-rework the select chain executed the
// Note case's emitStringEq on both values' UNDEF Note slots even though the
// active case is Empty. Post-rework the compare runs only under tag==Note.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_record_choice_field_payload_free_equality_pin) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "record Wrap(M: Msg); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Msg.Empty); "
        "  var b := new Wrap(Msg.Empty); "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // two Empty values are equal (§35.7)
}

// ─── String-payload choice equality through a record component: same tag +
// same content → equal; same tag + different payload → unequal; different tag
// → unequal (and the different-tag compare must not touch either value's
// undef slots). NB: every string literal occurrence lowers to its OWN global
// (.bz.str.N — no dedup), so two `"hello"` payloads have DISTINCT pointers
// and pin content equality vs pointer identity. Literals (sentinel rc) are
// used instead of concat-minted strings deliberately: a +1 string stored as a
// choice payload is never released at scope end — a PRE-EXISTING #2045-era
// disposal gap unrelated to equality (#2115) —
// and would trip LSan in these runAotChecked tests.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_record_choice_field_string_payload_equality) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "record Wrap(M: Msg); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Msg.Note(\"hello\")); "  // own literal global
        "  var b := new Wrap(Msg.Note(\"hello\")); "  // equal content, distinct ptr
        "  var c := new Wrap(Msg.Note(\"world\")); "
        "  var e := new Wrap(Msg.Empty); "
        "  if a = b and not (a = c) and not (a = e) and not (e = c) "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // content equality, not pointer identity
}

// ─── Finding 2: the DIRECT bare-choice operator surface. `a = b` on
// choice-typed operands is sema-legal for a value-like choice (§35.7); the
// pre-rework CG29b operand scan recognized only Record declarations, so this
// fell to emitStructEquality's shape-compare INCLUDING the undef inactive
// slots (int payloads: undef-poisoned result; string payloads: undef ptr
// compare). Post-rework it dispatches emitChoiceEquality.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_bare_choice_direct_eq_int_payload) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "class C public static method Main(): int "
        "  var a := Color.Green(7); "
        "  var b := Color.Green(7); "
        "  var c := Color.Green(8); "
        "  var x := Color.Red; "
        "  var y := Color.Red; "
        "  if a = b and not (a = c) and not (a = x) and x = y "
        "     and a <> c and not (a <> b) "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // §35.7: tag+active-payload equality
}

// Finding 2, string payload: the bare surface with a pointer payload — the
// shape-compare fallback would ptr-compare the string slots (content-equal
// but distinct strings unequal) AND read the undef Empty placeholder.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_bare_choice_direct_eq_string_payload) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"hello\"); "
        "  var b := Msg.Note(\"hello\"); "
        "  var e := Msg.Empty; "
        "  var f := Msg.Empty; "
        "  if a = b and e = f and not (a = e) "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Finding 2, direct `.Equals()` / `.GetHashCode()` value-semantics surface on
// a bare choice receiver (sema-legal for value-like choices, 80e59dc).
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_bare_choice_equals_and_gethashcode_methods) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"xy\"); "
        "  var b := Msg.Note(\"xy\"); "
        "  var e := Msg.Empty; "
        "  if a.Equals(b) and not (a.Equals(e)) "
        "     and a.GetHashCode() = b.GetHashCode() "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ─── Record-with-string payload: a choice case whose payload is a RECORD
// containing a string. The per-payload compare recurses through
// emitStructEquality → emitStringEq inside the case's arm; pre-rework that
// whole recursion ran under undef when the other case was active.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_record_with_string_payload_choice_equality) {
    JitResult r = runAotChecked(
        "module M; "
        "record Name(First: string); "
        "choice P Tagged(n: Name), Anon; end P; "
        "record W(V: P); "
        "class C public static method Main(): int "
        "  var a := new W(P.Tagged(new Name(\"ada\"))); "
        "  var b := new W(P.Tagged(new Name(\"ada\"))); "
        "  var c := new W(P.Tagged(new Name(\"bob\"))); "
        "  var d := new W(P.Anon); "
        "  var e := new W(P.Anon); "
        "  if a = b and not (a = c) and not (a = d) and d = e "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ─── Nested choice payload: Outer's case payload is ANOTHER string-bearing
// choice. The per-payload equality recurses emitChoiceEquality-in-
// emitChoiceEquality; each level's payload compare must stay inside its own
// tag arm.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_35_7_v210_nested_choice_string_payload_equality) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Inner Valued(text: string), Unit; end Inner; "
        "choice Outer First(inner: Inner), Second; end Outer; "
        "record Wrap(O: Outer); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Outer.First(Inner.Valued(\"k1\"))); "
        "  var b := new Wrap(Outer.First(Inner.Valued(\"k1\"))); "
        "  var c := new Wrap(Outer.First(Inner.Unit)); "
        "  var d := new Wrap(Outer.Second); "
        "  var e := new Wrap(Outer.Second); "
        "  if a = b and not (a = c) and not (a = d) and d = e "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ─── §3.6 / §8.3: equal values HASH EQUAL even when their inactive (undef)
// slots differ — the hash folds ONLY tag + the active case's payload. Distinct
// construction sites give the two values independently-undef Note slots for
// the Empty pair; content-equal but identity-distinct strings for the Note
// pair.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_8_3_v210_equal_choice_values_hash_equal) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "record Wrap(M: Msg); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Msg.Note(\"hello\")); "
        "  var b := new Wrap(Msg.Note(\"hello\")); "
        "  var e1 := new Wrap(Msg.Empty); "
        "  var e2 := new Wrap(Msg.Empty); "
        "  if a.GetHashCode() = b.GetHashCode() "
        "     and e1.GetHashCode() = e2.GetHashCode() "
        "     and a.GetHashCode() <> e1.GetHashCode() "
        "  then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ─── §3.6: Dictionary key whose record wraps a STRING-BEARING choice. Insert
// under one key, look up via an equal-content but identity-distinct key — the
// boxed hash thunk (emitRecordHashCode → emitChoiceHashCode) and the boxed
// equality thunk (emitStructEquality → emitChoiceEquality) must both run the
// active case's string content path, in control flow, inside the box thunks.
BEZEL_TEST(choice_equality_control_flow_rework_2083,
           spec_3_6_v210_dictionary_key_string_bearing_choice) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "record Wrap(M: Msg); "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Wrap, int>(); "
        "  var k1 := new Wrap(Msg.Note(\"k1\")); "
        "  var k2 := new Wrap(Msg.Empty); "
        "  d.Add(k1, 100); "
        "  d.Add(k2, 55); "
        "  var probe := new Wrap(Msg.Note(\"k1\")); "   // equal content, distinct identity
        "  var probe2 := new Wrap(Msg.Empty); "
        "  var v1 := -1; "
        "  var v2 := -1; "
        "  if d.TryGetValue(probe, v1) and d.TryGetValue(probe2, v2) "
        "  then return v1 + v2; end; "
        "  return -2; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 155);  // 100 + 55 (exit codes are 8-bit)
}
