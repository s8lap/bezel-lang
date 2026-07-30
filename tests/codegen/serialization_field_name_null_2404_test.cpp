// Codegen — issue #2404 / spec §9.8 ("Serialization synthesis") + stdlib
// §2.15 ("Concrete serialization formats"), docs revision v0.235.
//
// #2404's claim: `Emitter::emitRecordDeserialize` / `emitChoiceDeserialize`
// (LlvmBackend_Serialization.cpp) never null-check the pointer
// `bz_binder_read_field_name` returns before dereferencing it (unlike
// `emitClassDeserialize`'s explicit `errBB` null-check), so a truncated
// field-name read SIGSEGVs instead of raising `SerializationException`.
//
// VERIFY-FIRST finding: STALE for the crash itself, as of this lane's
// rebase (main includes #2399 @ 647fc6b, which itself sits on #2403 @
// 2ddbc0f + b00243b). #2403 already made `bz_binder_read_bz_string`
// (`bezel_runtime_serialization.cpp`) call `bz_ser_throw_at_binary` BEFORE
// every null return on a truncated length prefix or truncated payload —
// binary AND, transitively, JSON (whose `bz_jder_count_object_fields`
// already rejects an unterminated object before the field loop's first
// iteration). `bz_binder_read_field_name` delegates to that same helper for
// both its field-name role and every other string/char read, so the
// `emitPendingCheck()` already present immediately after every
// `bz_binder_read_field_name` call in `emitRecordDeserialize` /
// `emitChoiceDeserialize` now catches the SAME truncation the issue's own
// repro exercises — confirmed empirically with an exhaustive sweep (both
// formats, both record and choice, truncation cuts 1..100 against the
// issue's own two-string-field shape): zero crashes, every cut either
// throws `SerializationException` with the expected "Truncated ... (§2.15)"
// message or (once the cut length itself goes negative) throws the
// unrelated, pre-existing `IndexOutOfRangeException` from
// `AsReadOnlySpan`'s own bounds check — never a signal.
//
// The ONE residual gap `emitPendingCheck()` does NOT cover: a raw
// malloc/calloc failure INSIDE `bz_binder_read_bz_string` itself (its OOM
// paths return `nullptr` WITHOUT calling `bz_ser_throw_at_binary` — see the
// function body) still leaves a null pointer with no pending exception.
// `emitClassDeserialize` already defends this exact residual with a manual
// null-check-and-throw block (its comment says as much: "reached only for
// the OOM allocation-failure paths"); `emitRecordDeserialize` /
// `emitChoiceDeserialize` did not. This fix adds the same defensive
// null-check (throwing `SerializationException`) to both, for consistency
// and to close the asymmetry — a direct, deliberately narrower version of
// `emitClassDeserialize`'s `errBB`: since the #2398 disposeStack_ scope
// already registers every already-filled component/payload slot for
// automatic release (swept by `emitThrowBuiltin`'s `emitUnwindDisposals()`
// on any throw in this function, truncation-throws included — see that
// fix's own comments), no manual per-slot dispose loop is needed here,
// unlike the class path's.
//
// Per the project's OOM-hardening precedent (e.g.
// tests/runtime/dict_alloc_oom_hardening_2239_test.cpp, and the #2238/#2237/
// #2236 fixes it follows): direct malloc-failure injection is out of scope
// for a unit test, so this residual cannot be pinned with a literal
// fail-without/pass-with demonstration — the runtime-level fix (#2403,
// already landed) is what the tests below CAN mechanically re-verify: that
// every truncation offset the issue names (and a wide sweep around them)
// produces a clean `SerializationException`, never a crash, for record AND
// choice, binary AND JSON. This is an honest regression guard for the
// overall (runtime + codegen) mitigation, not a proof that the codegen-side
// null-check alone is exercised by these particular inputs.
//
// Adversarial grid covered (see the lane report for the full sweep):
//   * record vs choice; binary vs JSON.
//   * truncation landing in a field NAME's length prefix, in its data
//     bytes, and past the wire's total length (record/choice, both
//     formats, cuts 1..100 against a 2-string-field / 2-string-payload
//     shape) — all clean (no signal) via a standalone probe sweep
//     (scripts/agent_check.sh --probe --asan), not re-run per-cut here to
//     keep the gtest suite fast; the specific cuts below are the issue's
//     own named offsets plus the file/loop/clean-roundtrip cases the probe
//     sweep cannot exercise as a gtest.
//   * Deserialize called in a LOOP (class_primary_ctor_serialization_2325's
//     own shape family) — the #2399 lane's entry-block-vs-current-insertion-
//     point trap does not apply here (this fix adds no alloca, only a
//     conditional branch over the already-loaded `namePtr` SSA value), but
//     is re-run below as a mechanical check anyway.
//   * clean (non-truncated) round-trips for record and choice — the new
//     null-check must not disturb the happy path.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The issue's own exact repro (record, binary): a cut landing inside the
// SECOND field's own name length-prefix / name bytes. §2.15/§9.8 (v0.235):
// no crash — a clean SerializationException, no leak (runAotChecked checks
// both).
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           record_binary_field_name_truncation_no_crash_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 26; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false on a crash (WIFSIGNALED) or a leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The identical shape for a value-like CHOICE payload (binary): a cut
// landing inside the SECOND payload field's own name bytes.
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           choice_binary_payload_field_name_truncation_no_crash_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 26; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The JSON-format sibling: a truncated JSON object (missing its closing
// `}`, and everything after some point inside the second field). §2.15's
// `bz_jder_count_object_fields` throws BEFORE the field loop even starts —
// a different code path than the binary case, worth its own test rather
// than assuming symmetry. JsonSerializer's own compact output has no added
// whitespace, so this does not exercise the separate, still-open #2419
// "$case whitespace" bug (unrelated: no choice discriminator on this
// record-only test).
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           record_json_field_name_truncation_no_crash_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new JsonSerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The JSON-format choice sibling. Uses runAotCheckedLenient (crash/UAF/OOB
// check only, LSan OFF) rather than runAotChecked: a truncated JSON choice
// object always routes through `bz_binder_read_case_discriminator`'s own
// pre-count (`bz_jder_count_object_fields`), which has a SEPARATE,
// pre-existing "Bug-A" double-throw defect that orphans the first
// exception object on this exact path (confirmed via `git diff`/`git
// checkout` isolation to pre-exist independent of this fix) -- filed as
// #2427, out of scope for #2404 (a crash-prevention fix, not a leak fix).
// This test's OWN concern -- no crash -- is what runAotCheckedLenient
// checks; #2427's leak would otherwise fail it here for an unrelated
// reason.
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           choice_json_payload_field_name_truncation_no_crash_spec_9_8) {
    auto r = runAotCheckedLenient(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new JsonSerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false on a crash (WIFSIGNALED) or UAF/OOB
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Deserialize called in a LOOP: the exact shape
// class_primary_ctor_serialization_2325_test.cpp's own family pins, because
// synthesized Deserialize is inline-emitted at its call site — anything
// stateful the fix adds must be initialized at the CURRENT insertion point,
// not the entry block, or a second loop iteration reuses stale state (the
// #2399 lane's own UAF trap). This fix adds no alloca at all (only a
// conditional branch comparing the already-loaded `namePtr` SSA value
// against null), so the trap class does not apply — pinned here anyway as
// a mechanical check, three iterations, clean round-trips (no truncation).
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           record_deserialize_in_loop_no_stale_state_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..2 do "
        "    var p: Pair := new Pair(\"tag-a\", \"tag-b\"); "
        "    using var s := new BinarySerializer(); "
        "    p.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var back: Pair := Pair.Deserialize(d); "
        "    if back.first = \"tag-a\" and back.second = \"tag-b\" then sum := sum + 1; end; "
        "  end; "
        "  return (if sum = 3 then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean, full-read controls: the new null-check must not disturb the
// ordinary success path for either record or choice.
// ===========================================================================
BEZEL_TEST(serialization_field_name_null_2404,
           clean_record_roundtrip_unaffected_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Pair := Pair.Deserialize(d); "
        "  if back.first <> \"a-fairly-long-tag-value-string\" then return 1; end; "
        "  if back.second <> \"second-value-str\" then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Uses runAotCheckedLenient (crash/UAF/OOB check only, LSan OFF): a
// value-like choice CALL-RESULT local (`var v := Msg.Deserialize(d)`) with
// a string payload is never released at scope end -- a separate, pre-
// existing gap (#2115's own release mechanism admits only a direct
// choice-CASE-CONSTRUCTION initializer, not a call returning a choice;
// confirmed pre-existing via `git diff`/`git checkout` isolation) -- filed
// as #2428, out of scope for #2404 (a crash-prevention fix, not a leak
// fix). This test's OWN concern -- the new null-check does not disturb the
// ordinary success path -- is what runAotCheckedLenient checks; #2428's
// leak would otherwise fail it here for an unrelated reason.
BEZEL_TEST(serialization_field_name_null_2404,
           clean_choice_roundtrip_unaffected_spec_9_8) {
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
        "  return (case v of Text(var a, var b) when a = \"a-fairly-long-tag-value-string\" "
        "                                        and b = \"second-value-str\" => 42, "
        "                    _ => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
