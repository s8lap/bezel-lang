// Codegen — issue #2428 / spec §9.8 ("Serialization synthesis") + §35.4
// ("The classification") + §28.9 (runtime-managed string reference
// counting) + §28.3 (scope-end auto-drop), docs revision v0.235.
//
// #2115 added a scope-end release for a `var` value-like-choice local whose
// initializer MINTS its own +1 string payload -- but ONLY when the
// initializer is a DIRECT choice-case-construction expression
// (`valueLikeChoiceLocalMintsStringPayload`, LlvmBackend_Setup.cpp). A
// value flowing from an ORDINARY CALL that ITSELF constructs and returns
// the same shape -- most notably `var v := Msg.Deserialize(d);`, where
// every string payload is freshly read via `bz_binder_read_bz_string`
// (rc=1 from the runtime, exactly like a choice-case construction) -- was
// never recognized, because the initializer is an ordinary method-call
// CallExpr whose callee resolves to a MethodSymbol, not a
// ChoiceCaseSymbol. This is the exact shape #1747 fixed for RECORDS
// (`var p := Mk();`, a record-returning CALL) -- but no choice-side
// counterpart to #1747 was ever built.
//
// VERIFY-FIRST: reproduced the issue's own repro exactly against this
// lane's rebase (both the non-serialization construction-then-call-through
// form and the serialization round-trip form) -- confirmed leaking under
// LSan, matching the issue's own reported byte counts.
//
// SCOPE DECISION (see the lane report for the full analysis): #1747's OWN
// record-side gate admits ANY CallExpr returning the record type, "not the
// callee's own body shape" -- and scoping THIS fix surfaced that #1747's
// gate is UNSOUND for a callee that returns a BORROWED value (e.g. a plain
// field read) rather than a fresh construction: an empirically-confirmed
// heap-use-after-free, filed separately as #2448 (pre-existing, unrelated
// to this fix, NOT fixed here). To avoid extending the SAME unsound
// pattern to choices, this fix does NOT mirror #1747's general gate.
// Instead `isSynthesizedValueLikeChoiceDeserializeCall`
// (LlvmBackend_Setup.cpp) admits ONLY a call to the type's own
// SYNTHESIZED `Deserialize` factory -- structurally recognized the same
// way LlvmBackend_Serialization.cpp's own call-site dispatch routes
// `Type.Deserialize(d)` to emitChoiceDeserialize (declares
// IDeserializable, no hand-written "Deserialize" method). A synthesized
// factory is compiler-generated and cannot return a borrowed field/param
// -- there is no user body that could do so -- so this narrower gate is
// sound without a whole-callee-body analysis. A hand-written `Deserialize`
// (which suppresses synthesis, §9.8) is explicitly excluded.
//
// The release itself reuses #2425's arbitrary-depth
// emitChoiceStringFieldReleases walker (the choiceStringFields Disposal)
// rather than a third ad hoc mechanism, per #2425's own filed
// recommendation.
//
// RESIDUAL (documented, NOT fixed here, filed as #2450): the shared
// StringEscapeWalker treats a `case` MATCH SCRUTINEE as an unconditional
// escape (by design, shared across every consumer of the walker -- string
// locals, #1747 records, this mechanism). Both of #2428's own reported
// repros use the local SOLELY as a case scrutinee
// (`case v of Text(var a, var b) => ...`), so neither is closed by this
// fix -- they stay the prior safe-leak behaviour, pinned as "no UAF" below
// rather than claimed fixed. This fix DOES close the shape #1747's own
// precedent targets: a call-result local that is not subsequently
// re-matched/re-passed (declared and simply not read again, or read only
// in a position the walker already recognizes as safe).
//
// Adversarial grid covered (full detail in the lane report):
//   * the fix's actual reach: an unused (declared, never re-read)
//     Deserialize call-result local -- LSan-clean pre-fix FAILS / post-fix
//     PASSES.
//   * the issue's own two repros (direct construction + case-match, and
//     the Deserialize + case-match round trip) -- pinned as "no crash / no
//     UAF", NOT as "leak-free" (the scrutinee-escape residual, #2450,
//     keeps them leaking by design of the shared conservative walker).
//   * `_ := v;` (explicit discard) -- ALSO stays a conservative escape
//     under the shared walker (an unconditional escape read, matching
//     every other DiscardStmt use across the whole walker, not specific
//     to this mechanism) -- pinned as "no crash / no UAF".
//   * a HAND-WRITTEN Deserialize (suppresses synthesis, §9.8) -- must NOT
//     be admitted by isSynthesizedValueLikeChoiceDeserializeCall; pinned
//     as "no crash" (the hand-written body here returns a fresh
//     literal-payload construction, so even a wrongly-armed release would
//     be a no-op on the sentinel literals -- this test pins the exclusion
//     path compiles and runs cleanly, not a UAF stress case for the
//     exclusion itself, which is structural and mirrors the codegen's own
//     proven dispatch predicate).

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The fix's actual reach: `var v := Msg.Deserialize(d);` declared and never
// subsequently read. The conservative escape walk proves this genuinely
// non-escaping, so both mechanisms (isSynthesizedValueLikeChoiceDeserializeCall
// + the shared #2425 release walker) arm cleanly. LSan-clean pre-fix FAILS
// (49 bytes / 2 allocations leaked -- confirmed via patch isolation,
// matching the issue's own reported byte count for its serialization
// repro); post-fix PASSES.
// ===========================================================================
BEZEL_TEST(choice_string_field_call_release_2428,
           deserialize_call_result_unused_local_released_spec_9_8_35_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 49-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The issue's own non-serialization repro: direct construction (already
// #2115-eligible, unrelated to THIS fix) matched via `case`. Pinned as
// "no crash / no UAF" -- the scrutinee-escape residual (#2450) keeps this
// leaking BOTH before and after this fix; runAotCheckedLenient disables
// leak detection so the pin is specifically about crash/UAF safety, not a
// (false) leak-free claim.
// ===========================================================================
BEZEL_TEST(choice_string_field_call_release_2428,
           direct_construction_case_match_no_uaf_residual_leak_spec_35_5) {
    auto r = runAotCheckedLenient(
        "module M; choice Msg Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text($\"hello-world-{1}\", $\"second-{2}\"); "
        "  return (case m of Text(var a, var b) => 42, _ => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // no crash / no UAF (generated-code ASan still active)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The issue's own serialization repro: `Msg.Deserialize(d)` matched via
// `case ... when ...`. Same residual as above (scrutinee escape) -- pinned
// as "no crash / no UAF", not leak-free.
// ===========================================================================
BEZEL_TEST(choice_string_field_call_release_2428,
           deserialize_case_match_no_uaf_residual_leak_spec_9_8_35_5) {
    auto r = runAotCheckedLenient(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Text(var a, var b) "
        "            when a = \"a-fairly-long-tag-value-string\" and b = \"second-value-str\" => 42, "
        "          _ => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// `_ := v;` (explicit discard) -- the shared walker treats EVERY
// DiscardStmt's value as an unconditional escape (not specific to this
// mechanism), so this stays the prior safe-leak behaviour too. Pins no
// crash / no UAF.
// ===========================================================================
BEZEL_TEST(choice_string_field_call_release_2428,
           deserialize_call_result_discarded_no_uaf_spec_9_8) {
    auto r = runAotCheckedLenient(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  _ := v; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// A HAND-WRITTEN Deserialize (suppresses synthesis, §9.8) must NOT be
// admitted by isSynthesizedValueLikeChoiceDeserializeCall -- its body is
// not provably a fresh construction in general (this one happens to
// construct-and-return a fresh, literal-sentinel-payload value, so even a
// wrongly-armed release would be a safe no-op here; the pin is that the
// exclusion path compiles and runs cleanly under full LSan).
// ===========================================================================
BEZEL_TEST(choice_string_field_call_release_2428,
           hand_written_deserialize_excluded_no_uaf_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; "
        "  public static method Deserialize(d: IDeserializer): Msg "
        "    return Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  end Deserialize; "
        "end Msg; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
