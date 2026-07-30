// Codegen — issues #2403 / #2394 / spec §2.14 (serialization vocabulary) +
// §2.15 (concrete serialization formats) + §9.8 (serialization synthesis) +
// §28.3/§28.9 (exactly-once reclamation), docs revision v0.235.
//
// VERIFY-FIRST findings (scripts/agent_check.sh --probe --asan, against
// pre-fix main @ a1bdb4c, i.e. AFTER #2398 landed but before this fix):
//
//   * #2403's own repro (`Pair(first: string, second: string)`, cutLen =
//     bytes.Length - 5, landing inside `second`'s own UTF-8 payload bytes)
//     returns exit 7 (the program's own "no exception" branch), NOT 42 —
//     `Pair.Deserialize` returns SUCCESSFULLY with `second` silently null,
//     confirmed to SIGSEGV if the caller then touches `second` (e.g.
//     `.Length`) as an ordinary non-nullable `string` field would never be.
//   * A cut landing inside `second`'s own 4-byte LE32 length PREFIX (rather
//     than its payload) reproduces identically: exit 7, no exception.
//   * #2394's `bool` repro (`Flags(Active: bool, Value: bool)`, cutting the
//     trailing byte) decodes `Value` as `false` and returns successfully —
//     no exception, no crash, just a silently wrong default.
//   * #2394's `decimal` repro (`D(X: int, V: decimal)`, cutting the
//     trailing 4-byte LE32 word — the decimal's flags/scale word) decodes
//     `V` as `314` instead of `3.14` and returns successfully — silently
//     WRONG DATA (not a missing/default value: the truncated flags word
//     zeroes the scale, changing the represented magnitude), no exception.
//
// Root cause (confirmed by reading bz_binder_read_bz_string / _bool /
// _decimal in src/runtime/bezel_runtime_serialization.cpp before the fix):
// unlike bz_binder_read_int/_uint/_float (which call bz_ser_throw_at_binary
// on the identical truncation shape — fixed for #688), these three readers
// returned a default/zero/null value from a truncated
// bz_binder_read_le32_impl/bz_binder_read_byte_impl WITHOUT calling
// bz_ser_throw at all — not even an unclaimed pending exception for a later
// emitPendingCheck() to catch (unlike #2378's Bug A, where an exception WAS
// raised but not checked promptly). Fixed by mirroring the scalar readers:
// capture the start-of-value byte offset and call bz_ser_throw_at_binary on
// the failing read.
//
// #2394 asked "is this the same bug as #2403?" — yes, confirmed empirically
// (see above): identical root cause (a binary-mode reader silently
// defaulting instead of raising on truncation) across bz_binder_read_bool,
// bz_binder_read_decimal, and bz_binder_read_bz_string (the shared string
// reader used by ReadString/ReadChar/field names/discriminators). Fixed
// together in one commit since the defect and the fix are identical across
// the three functions.
//
// Interaction with #2398 (landed just before this fix, same day): #2398 gave
// emitRecordDeserialize/emitChoiceDeserialize a disposal scope so an
// already-read string SIBLING is released when a LATER component's read
// throws — but since a truncated trailing STRING value never threw before
// THIS fix, that scope was never exercised by a string-value-truncation
// throw specifically (only by an int/nested-type truncation, per that
// issue's own test file comment: "a truncation landing directly inside a
// trailing STRING value never raises at all ... so `c: int` is what
// reliably drives the exception here"). This file's leak tests close that
// gap: the disposal scope IS exercised correctly when the NEW string/bool/
// decimal throws fire (verified via runAotChecked, no leak).
//
// Not independently re-confirmed here, but observed as an incidental
// byte-offset-sweep side effect: cuts landing inside the trailing field's
// own NAME bytes (rather than its value) — the #2404 SIGSEGV region — also
// now raise SerializationException instead of crashing, since
// bz_binder_read_field_name's binary path calls the same
// bz_binder_read_bz_string this fix changes, and emitRecordDeserialize
// already had an unconditional emitPendingCheck() after that call (#2378)
// that simply had nothing to check before now. #2404 is a distinct issue
// with its own owner; this file does not add a test asserting on that
// region since claiming or exercising a fix belongs there, not here — noted
// for whoever re-verifies it.
//
// Also discovered during verify-first (filed separately, #2408, NOT fixed
// here — different root cause, out of scope): SerializationException.Offset
// (stdlib §2.15, `long?`) reads back garbage instead of the correct byte
// offset, reproducing identically on the pre-existing #2378/#688 `int`
// truncation path — so this file asserts the offset via `.Message`
// (`Contains("byte offset")`), matching the existing
// serialization_688_posinfo_test.cpp convention, not via `.Offset` directly.
//
// Spec citations (docs v0.235):
//   * stdlib §2.14 "Serialization vocabulary": `ReadBool`/`ReadString`/
//     `ReadDecimal` are declared in the same `IDeserializer` interface as
//     `ReadInt`/`ReadUInt`/`ReadFloat`.
//   * stdlib §2.15 "Concrete serialization formats", **Errors**: "Malformed
//     input, type mismatches ..., and depth-limit violations raise
//     SerializationException" and "every other read member throws on
//     mismatch rather than returning a sentinel, because a deserialization
//     mismatch is a structural error in the input, not an expected
//     control-flow branch."
//   * stdlib §2.15, **Binary mapping**: "The compact binary format is
//     length-prefixed" — an unsatisfiable length prefix (too few bytes for
//     the prefix itself, or a declared byte count exceeding the remaining
//     buffer) is definitionally malformed input under the Errors rule above.
//
// FAILS WITHOUT / PASSES WITH: every test below whose comment cites an
// observed pre-fix exit code / decoded value (7, `false`, `314`, or a
// leak size) flips to the asserted post-fix behavior (confirmed via
// scratch-copy `git diff` of src/runtime/bezel_runtime_serialization.cpp
// applied/reverted with `git apply`/`git checkout` — NOT git stash — +
// rebuild + re-run, then restored). See the fix commit message for the
// full mechanical-proof transcript.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Preamble shared by every test: BinarySerializer/BinaryDeserializer require
// `import System.Serialization` (stdlib §2.15).
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// #2403: a truncated trailing STRING value now raises, both for a cut
// landing in the value's own PAYLOAD and a cut landing in the value's own
// LENGTH PREFIX. This is the issue's own exact repro shape
// (`Pair(first: string, second: string)`).
// ===========================================================================

// Payload truncation: cutLen = bytes.Length - 5 lands 5 bytes short, inside
// `second`'s own 16-byte UTF-8 payload ("second-value-str"). Pre-fix: exit 7
// (silently "succeeded" with second == null). Post-fix: raises, message
// names the payload specifically.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_value_payload_truncation_raises_spec_2_15) {
    auto r = runAot(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated binary input reading string data\") "
        "            and e.Message.Contains(\"byte offset\") then 42 else 2); "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Length-prefix truncation: cutLen = bytes.Length - 18 lands inside
// `second`'s own 4-byte LE32 length prefix (fewer than 4 bytes remain to
// decode it). Pre-fix: exit 7, identically to the payload case. Post-fix:
// raises, message names the length prefix specifically (a DIFFERENT message
// than the payload case, proving the fix distinguishes the two failure
// sites the way bz_binder_begin_object's own truncated-header throw does).
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_value_length_prefix_truncation_raises_spec_2_15) {
    auto r = runAot(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 18; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated binary input reading string length\") "
        "            and e.Message.Contains(\"byte offset\") then 42 else 2); "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Same payload-truncation repro through the plain (no-ASan) runAot harness,
// pinning the issue's own reported exit code directly: 7 pre-fix, 42
// post-fix (via a `caught` bool exactly matching the issue's own repro
// shape, rather than inspecting the message).
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_value_truncation_no_longer_silently_succeeds_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 7; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);  // 7 before the fix (silently "succeeded" with second == null)
}

// ===========================================================================
// #2394: a truncated trailing BOOL value now raises instead of silently
// decoding as `false`.
// ===========================================================================
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           bool_value_truncation_raises_spec_2_15) {
    auto r = runAot(
        std::string("module M; ") + kImport +
        "record Flags(Active: bool, Value: bool): ISerializable, IDeserializable "
        "end Flags; "
        "class C public static method Main(): int "
        "  var p: Flags := new Flags(true, true); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 1; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  try "
        "    var back: Flags := Flags.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated binary input reading bool\") "
        "            and e.Message.Contains(\"byte offset\") then 42 else 2); "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Plain-harness pin: pre-fix, `Value` silently decoded as the wrong value
// `false` (the truncated byte read leaves the destination at its `uint8_t
// b = 0` initializer) and the program returned successfully; post-fix it
// raises. `Value` was constructed `true`, so a silent "success" here is
// caught by the wrong-boolean check as well as the missing exception.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           bool_value_truncation_no_longer_silently_wrong_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Flags(Active: bool, Value: bool): ISerializable, IDeserializable "
        "end Flags; "
        "class C public static method Main(): int "
        "  var p: Flags := new Flags(true, true); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 1; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Flags := Flags.Deserialize(d); "
        "    return (if back.Value then 3 else 7); "  // 7 pre-fix: silently decoded false
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);
}

// ===========================================================================
// #2394: a truncated trailing DECIMAL value now raises instead of silently
// decoding as a numerically WRONG value (the truncated flags/scale word
// changes the represented magnitude, not just zeroing/defaulting it).
// ===========================================================================
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           decimal_value_truncation_raises_spec_2_15) {
    auto r = runAot(
        std::string("module M; ") + kImport +
        "record D(X: int, V: decimal): ISerializable, IDeserializable end D; "
        "class C public static method Main(): int "
        "  var p: D := new D(5, 3.14m); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 4; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  try "
        "    var back: D := D.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Truncated binary input reading decimal\") "
        "            and e.Message.Contains(\"byte offset\") then 42 else 2); "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Plain-harness pin: pre-fix, cutting the trailing 4-byte LE32 word (the
// decimal's flags/scale word) left it zeroed, decoding 3.14m as the WRONG
// value 314m (scale silently dropped to 0) with no exception. Post-fix:
// raises. Directly asserts the pre-fix VALUE was wrong, not merely absent.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           decimal_value_truncation_no_longer_silently_wrong_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record D(X: int, V: decimal): ISerializable, IDeserializable end D; "
        "class C public static method Main(): int "
        "  var p: D := new D(5, 3.14m); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 4; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: D := D.Deserialize(d); "
        "    return (if back.V = 314m then 7 else 3); "  // 7 pre-fix: silently wrong 314m
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"),
        42);
}

// ===========================================================================
// Self-pass (c): a legitimately empty string still round-trips (the new
// truncation check must not fire on a zero-length payload — `d->pos +
// (int64_t)byte_len > d->len` with byte_len == 0 is never true just because
// the payload itself is empty; only an actually-short buffer trips it).
// ===========================================================================
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           zero_length_string_field_round_trips_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Tag(Label: string): ISerializable, IDeserializable end Tag; "
        "class C public static method Main(): int "
        "  var p: Tag := new Tag(\"\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Tag := Tag.Deserialize(d); "
        "  if back.Label <> \"\" then return 1; end; "
        "  if back.Label.Length <> 0 then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Self-pass (c) continued / clean round-trip controls: the exact shapes
// used in the truncation tests above round-trip correctly (VALUE-asserting,
// not just pass/fail) when NOT truncated, and — via runAotChecked — do not
// themselves leak on the ordinary success path.
// ===========================================================================
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           clean_round_trip_two_string_fields_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
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

BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           clean_round_trip_bool_fields_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Flags(Active: bool, Value: bool): ISerializable, IDeserializable "
        "end Flags; "
        "class C public static method Main(): int "
        "  var p: Flags := new Flags(true, false); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Flags := Flags.Deserialize(d); "
        "  if back.Active <> true then return 1; end; "
        "  if back.Value <> false then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           clean_round_trip_decimal_field_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record D(X: int, V: decimal): ISerializable, IDeserializable end D; "
        "class C public static method Main(): int "
        "  var p: D := new D(5, 3.14m); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: D := D.Deserialize(d); "
        "  if back.X <> 5 then return 1; end; "
        "  if back.V <> 3.14m then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Self-pass (a) / (d): does raising now leak the partially-read value, and
// does it interact correctly with #2398's sibling-disposal scope? A record
// with an already-successfully-read STRING sibling, followed by a field
// whose truncated read now throws via THIS fix (string / bool / decimal in
// turn) — verified leak-clean via runAotChecked, exercising the exact
// scope #2398 added but that a string/bool/decimal-value truncation could
// never reach before this fix landed.
// ===========================================================================

// String sibling orphaned by a LATER STRING field's own value truncation —
// the #2403 repro shape itself, re-asserted under ASan/LSan.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_sibling_disposed_when_second_string_value_truncates_no_leak_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 5; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Pair := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix-of-this-issue: `first` orphaned (no throw at all pre-#2403-fix, so #2398's scope was never even entered on this shape; post-fix-of-this-issue it IS entered and must not leak)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// String sibling orphaned by a LATER BOOL field's own value truncation.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_sibling_disposed_when_bool_value_truncates_no_leak_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record TaggedFlag(Tag: string, Flag: bool): ISerializable, IDeserializable "
        "end TaggedFlag; "
        "class C public static method Main(): int "
        "  var p: TaggedFlag := new TaggedFlag(\"a-fairly-long-tag-value-string\", true); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 1; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: TaggedFlag := TaggedFlag.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// String sibling orphaned by a LATER DECIMAL field's own value truncation.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_sibling_disposed_when_decimal_value_truncates_no_leak_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record TaggedAmount(Tag: string, Amount: decimal): ISerializable, IDeserializable "
        "end TaggedAmount; "
        "class C public static method Main(): int "
        "  var p: TaggedAmount := new TaggedAmount(\"a-fairly-long-tag-value-string\", 3.14m); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 4; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: TaggedAmount := TaggedAmount.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// String sibling orphaned by a LATER string field's own LENGTH-PREFIX
// truncation specifically (as opposed to its payload, above) — the second
// of the two #2403 failure sites, re-asserted under ASan/LSan.
BEZEL_TEST(serialization_string_bool_decimal_truncation_2403_2394,
           string_sibling_disposed_when_second_string_length_prefix_truncates_no_leak_spec_28_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Pair(first: string, second: string): ISerializable, IDeserializable "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p: Pair := new Pair(\"a-fairly-long-tag-value-string\", "
        "                          \"second-value-str\"); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 18; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
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
