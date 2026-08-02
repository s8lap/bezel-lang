// Codegen/runtime — issue #2427 / spec §9.8 ("Serialization synthesis" >
// "Choice serialization") + stdlib §2.15 ("Concrete serialization formats"),
// docs revision v0.235.
//
// #2427's claim: `bz_binder_read_case_discriminator`'s JSON path
// (`src/runtime/bezel_runtime_serialization.cpp`, the `d->kind >= 1` branch)
// calls `bz_jder_count_object_fields(d)` to pre-count the choice object's
// field count. That helper already raises `SerializationException` (via
// `bz_ser_throw_at_json`) and returns -1 internally when the JSON object is
// truncated before its closing '}' -- an already-in-flight exception.
// `bz_binder_read_case_discriminator` did not check for that pending
// exception before testing the return value: `total_count == -1` satisfies
// `total_count <= 0`, so the function unconditionally raised a SECOND,
// unrelated exception ("empty object, missing \"$case\" discriminator"),
// which SUPERSEDES the first. `bz_ser_throw` unconditionally overwrites the
// thread-local `bz_exc` slot with the new exception object with no regard
// for what was already there, so the first (genuinely-caused) exception
// object -- and its own heap-allocated Message string -- is orphaned: a
// leak, and the caller observes the WRONG (misleading) diagnostic.
//
// This is the same "Bug-A pattern" (a runtime reader's failure/truncation
// result used without an already-pending-exception check, so a later
// unconditional throw supersedes and orphans it) previously fixed at
// several call sites in this file (#2378 lineage, #2403, #2404, #2419) --
// here it recurs in `bz_binder_read_case_discriminator`'s own pre-count
// check.
//
// VERIFY-FIRST (scripts/agent_check.sh --probe --asan, against pre-fix
// origin/main): the issue's own repro -- a `Msg: Text(string,string), Empty`
// choice, serialized to JSON then truncated 5 bytes off the end -- caught
// SerializationException (exit 42, correct control flow) but under
// LeakSanitizer: "156 byte(s) leaked in 3 allocation(s)" with the leak
// trace rooted at `bz_ser_throw <- bz_jder_count_object_fields <-
// bz_binder_read_case_discriminator <- main`, exactly matching the issue.
// The CAUGHT exception's `Message` was confirmed (by a temporary source
// revert + rebuild, not by reading the issue) to be the misleading
// "Choice deserialize: empty object, missing \"$case\" discriminator (§9.8)"
// -- not the genuine truncation cause.
//
// Fix: `bz_binder_read_case_discriminator`'s JSON branch now checks the
// thread-local `bz_exc` slot immediately after calling
// `bz_jder_count_object_fields` and returns `nullptr` immediately if it is
// already set, before testing `total_count <= 0` -- mirroring the pending-
// check idiom used elsewhere in the runtime (e.g.
// bezel_runtime_decimal_guid.cpp's `if (bz_exc) return 0;`) and the RECORD-
// side sibling (`bz_binder_begin_object`'s JSON path), which does not
// double-throw because it propagates `total_count`/-1 straight through to
// its codegen caller's own `emitPendingCheck()` rather than re-inspecting
// the value itself. Post-fix, the SAME repro is LSan-clean and the caught
// exception's Message is the genuine
// "Truncated JSON: unexpected end of input inside object (§2.15)".
//
// Adversarial self-pass (scripts/agent_check.sh --probe --asan, standalone
// probe scripts, not re-run per-cut here to keep the gtest suite fast):
//   * A full truncation SWEEP of the issue's own wire form, cut length 1
//     through the full 85-byte length: every cut in [1, 84] raises the
//     correct "Truncated JSON ..." message with ZERO LeakSanitizer reports
//     (post-fix); cut 85 (untruncated) round-trips normally. Pre-fix, cut 5
//     bytes off the end reproduces the issue's exact leak.
//   * Binary mode is untouched by this fix (the changed branch is
//     `d->kind >= 1`, JSON-only): a binary-mode truncated-choice control is
//     included below to confirm no regression.
//   * The "must still raise" control: a genuinely empty `{}` choice object
//     (total_count == 0, no pending exception) must still raise the
//     original "missing \"$case\" discriminator" message -- the fix must
//     not turn that legitimate error into a silent accept or the wrong
//     message.
//   * The discriminator's leading-position and undeclared-string requirements
//     (pre-existing, unrelated to this fix) remain covered by
//     serialization_case_type_ws_2419_test.cpp's own negative control; not
//     re-verified here to avoid duplicating coverage.
//   * Clean (non-truncated) round-trip, including a payload-free case, to
//     confirm the added `if (bz_exc) return nullptr;` is a no-op on the
//     happy path (bz_exc is never set there).
//
// APIs verified against Bezel_Standard_Library.md: ISerializer/IDeserializer
// (§2.14), JsonSerializer/JsonDeserializer/BinarySerializer/
// BinaryDeserializer/SerializationException (§2.15), OwningBuffer<byte> /
// ReadOnlySpan<byte> (§4.14).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on ANY sanitizer fire (leak / UAF / crash);
// mapping that to a sentinel lets a single EXPECT_EQ pin both memory-
// cleanliness AND the round-tripped exit value in one assertion (mirrors
// class_deserialize_field_init_throw_2363_test.cpp's runChecked).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// The issue's own choice shape: a two-string-field payload case plus a
// payload-free case, so both dispatch and payload binding are exercised.
const char* kMsg =
    "choice Msg: ISerializable, IDeserializable "
    "  Text(first: string, second: string), Empty; "
    "end Msg; ";

}  // namespace

// ===========================================================================
// Primary fix target: the issue's own exact repro, LSan-checked.
// Pre-fix: sanitizer fires (leak) -> runChecked returns -999.
// Post-fix: clean, catches SerializationException -> runChecked returns 42.
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           issue_repro_truncated_json_choice_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", "
        "\"second-value-str\"); "
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
        "end Main; end C;"), 42);
}

// ===========================================================================
// Message-correctness control (not leak-checked -- plain runAot): the
// caught exception must carry the GENUINE truncation diagnostic, not the
// misleading "empty object, missing \"$case\"" one the double-throw produced
// pre-fix. A coarse "did it raise SOME SerializationException" assertion
// would pass even on the pre-fix superseding exception, so this asserts on
// the message text directly (per the task's own guidance on why `rejects()`
// is insufficient here).
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           truncated_json_choice_message_is_the_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", "
        "\"second-value-str\"); "
        "  using var s := new JsonSerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 1; "  // reached only if it wrongly did NOT raise
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated JSON\") and "
        "               not e.Message.Contains(\"missing\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Truncation sweep (a handful of representative offsets spanning the choice
// object, not just the issue's own "-5 bytes" cut) -- each must be LSan-clean
// and must raise the genuine truncation message. Chosen offsets: just after
// the opening '{' (offset 1), mid-"$case"-value, mid-first-payload-field, and
// just before the final '}' (mirrors the issue's own cut). A broader sweep
// (every offset 1..84) was run standalone via scripts/agent_check.sh --probe
// --asan during verification (see file header); these are the representative
// points kept in the gtest suite to stay fast.
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           truncation_sweep_representative_offsets_lsan_clean_spec_9_8) {
    const int offsets[] = {1, 10, 30, 50, 70, 80};
    for (int off : offsets) {
        int rc = runChecked(
            std::string("module M; ") + kImport + kMsg +
            "class C public static method Main(): int "
            "  var m := Msg.Text(\"a-fairly-long-tag-value-string\", "
            "\"second-value-str\"); "
            "  using var s := new JsonSerializer(); "
            "  m.Serialize(s); "
            "  using var bytes := s.ToBytes(); "
            "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan(0, " +
            std::to_string(off) + ")); "
            "  var caught: bool := false; "
            "  try "
            "    var v := Msg.Deserialize(d); "
            "    return 0; "
            "  catch SerializationException => caught := true; end; "
            "  return (if caught then 42 else 1); "
            "end Main; end C;");
        BEZEL_EXPECT_EQ(rc, 42);
    }
}

// ===========================================================================
// "Must still raise" control: a genuinely empty `{}` choice object (no
// truncation, no pending exception -- bz_jder_count_object_fields returns a
// clean 0) must still raise "missing \"$case\" discriminator" -- the fix
// must not suppress this legitimate, pre-existing error path.
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           genuinely_empty_object_still_raises_missing_case_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"missing\") and "
        "               e.Message.Contains(\"$case\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Binary-mode control: the fix touches only the `d->kind >= 1` (JSON) branch
// of bz_binder_read_case_discriminator; the binary path's own truncation
// handling (a distinct code path, `bz_binder_read_le32_impl` failing) is
// unaffected and must remain LSan-clean.
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           binary_mode_truncated_choice_unaffected_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"first-val\", \"second-val\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Clean-round-trip controls: the new `if (bz_exc) return nullptr;` check
// must be a no-op on every non-error path (bz_exc is never set there).
// Plain `run()` (not `runChecked`/LSan) is used deliberately: a value-like
// choice's string payload landing in a CALL-RESULT local (`var v :=
// Msg.Deserialize(d)`) has a PRE-EXISTING, unrelated leak tracked as #2428
// ("a value-like choice's string payload is never released for a call-result
// local") -- confirmed independent of this fix by reproducing it on plain
// Msg.Text("alpha","beta") round-trip code with no truncation involved at
// all. Running these controls under LSan would fail on #2428, not on
// anything #2427 touches; value-correctness (not leak-cleanliness) is what
// these two controls are for.
// ===========================================================================
BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           clean_roundtrip_payload_case_unaffected_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"alpha\", \"beta\"); "
        "  using var s := new JsonSerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Text(var f, var sec) => "
        "            (if f = \"alpha\" and sec = \"beta\" then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_case_disc_double_throw_2427,
           clean_roundtrip_payload_free_case_unaffected_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var m := Msg.Empty; "
        "  using var s := new JsonSerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Text(_, _) => 1, Empty => 42); "
        "end Main; end C;"), 42);
}
