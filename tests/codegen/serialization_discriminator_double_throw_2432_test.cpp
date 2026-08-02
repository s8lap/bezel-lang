// Codegen/runtime — issue #2432 / spec §9.8 ("Serialization synthesis" >
// "Choice serialization" / "Polymorphic dispatch") + stdlib §2.15 ("Concrete
// serialization formats"), docs revision v0.235.
//
// #2432's claim: #2427 fixed exactly ONE Bug-A double-throw call site
// (bz_binder_read_case_discriminator's `bz_jder_count_object_fields` pre-
// count check). Auditing the same function and its structurally-identical
// sibling `bz_binder_read_poly_discriminator` (src/runtime/
// bezel_runtime_serialization.cpp, JSON branch, `d->kind >= 1`) turned up
// FIVE more instances of the same pattern: a reader sub-call
// (`bz_jder_expect_char` / `bz_jder_parse_string`) that already raises
// SerializationException internally and returns a failure sentinel, whose
// result is then tested WITHOUT a pending-exception check before an
// unconditional `bz_ser_throw` that supersedes/orphans the genuine one
// (leak: the first exception object, including its own heap-allocated
// Message string).
//
//   1. bz_binder_read_case_discriminator, `bz_jder_expect_char(d, ':')` after
//      "$case" (line ~2278 pre-fix).
//   2. bz_binder_read_case_discriminator, `bz_jder_parse_string(d)` for the
//      "$case" value (line ~2292 pre-fix).
//   3. bz_binder_read_poly_discriminator, `bz_jder_count_object_fields(d)`
//      pre-count (line ~2084 pre-fix) — identical shape to #2427's own fix,
//      for "$type" instead of "$case".
//   4. bz_binder_read_poly_discriminator, `bz_jder_expect_char(d, ':')` after
//      "$type" (line ~2124 pre-fix).
//   5. bz_binder_read_poly_discriminator, `bz_jder_parse_string(d)` for the
//      "$type" value (line ~2135 pre-fix).
//
// VERIFY-FIRST / fail-without-fix demonstration: each BEZEL_TEST below was
// run against a temporarily-reverted (pre-#2432-fix) copy of
// bezel_runtime_serialization.cpp via scripts/agent_check.sh — every
// "double_throw" test below FAILED (wrong/superseded message and, for the
// runChecked() cases, an LSan leak report) before the fix, and PASSES after.
// Sites 1 and 3 (case ':' and poly count) plus case's parse_string (site 2)
// and case's own '"$case"' truncation shape were independently LSan-
// CONFIRMED by the issue's own repro bytes; sites 4 and 5 (poly ':' and poly
// parse_string) are exercised here with the SAME mechanism (a still-pending
// `bz_exc` at the point of the second throw) as their LSan-confirmed "$case"
// siblings, and this file's own fail-before/pass-after run against the
// reverted source is this lane's reachability confirmation for them.
//
// Fix (mirrors #2427 exactly): each site now checks the thread-local
// `bz_exc` slot (declared in bezel_runtime.h, visible via
// bezel_runtime_internal.hpp) immediately after the throwing sub-call and
// bails out (`return nullptr;`) before any manual re-throw, so an already-
// in-flight exception is never superseded. For the two
// `bz_jder_expect_char` sites (1, 4) this is unconditional (expect_char
// always raises on failure); for the two `bz_jder_parse_string` sites (2, 5)
// the manual throw remains reachable as the safety net for
// `bz_jder_cps_to_bz_string`'s untested OOM path, which returns nullptr
// WITHOUT having thrown.
//
// Spec citations verified against the actual heading text (v0.235):
//   * spec §9.8 "Serialization synthesis" > "Choice serialization": the
//     discriminator is read first and an object that does not lead with it,
//     or is malformed, raises SerializationException.
//   * spec §9.8 "Polymorphic dispatch": the "$type" discriminator is read
//     "first, before any component of the object" — the structurally
//     identical sibling this fix also covers.
//   * stdlib §2.15 "Concrete serialization formats": the JSON mapping and
//     the `$`-reserved field set (`$id`, `$ref`, `$type`, `$case`).
//
// APIs verified against Bezel_Standard_Library.md: ISerializer/IDeserializer
// (§2.14), JsonSerializer/JsonDeserializer/SerializationException (§2.15).
//
// Issue #2432.

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

// runAotChecked returns ran=false on ANY sanitizer fire (leak / UAF /
// crash); mapping that to a sentinel lets a single EXPECT_EQ pin both
// memory-cleanliness AND the round-tripped exit value (mirrors
// serialization_case_discriminator_double_throw_2427_test.cpp's runChecked).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// The "$case" choice shape, matching #2419's own kMsg (Both/Empty), so the
// discriminator's dispatch context matches the sibling files' established
// examples.
const char* kMsg =
    "choice Msg: ISerializable, IDeserializable "
    "  Both(first: string, second: int), Empty; "
    "end Msg; ";

// The "$type" polymorphic base/leaf shape, matching #2419's own Base2/Leaf2.
const char* kPoly =
    "abstract class Base2: ISerializable, IDeserializable "
    "  polymorphic(\"leaf\": Leaf2); "
    "  public abstract method Tag(): int; "
    "end Base2; "
    "class Leaf2 : Base2, ISerializable, IDeserializable "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public override method Tag(): int return 7; end Tag; "
    "end Leaf2; ";

}  // namespace

// ===========================================================================
// Site 1: bz_binder_read_case_discriminator, bz_jder_expect_char(':') after
// "$case". LSan-confirmed by the issue: a non-colon byte where the ':'
// should be, brace-balanced so bz_jder_count_object_fields's pre-scan does
// not intercept it first.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           case_colon_missing_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\"X\\\"Both\\\", \\\"first\\\": "
        "\\\"a\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           case_colon_missing_message_is_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\"X\\\"Both\\\", \\\"first\\\": "
        "\\\"a\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Unexpected JSON token\") and "
        "               not e.Message.Contains(\"expected ':'\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Site 2: bz_binder_read_case_discriminator, bz_jder_parse_string for the
// "$case" value. LSan-confirmed by the issue: an unknown escape sequence in
// the "$case" value.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           case_value_bad_escape_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\": \\\"\\\\z\\\", \\\"first\\\": "
        "\\\"a\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           case_value_bad_escape_message_is_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$case\\\": \\\"\\\\z\\\", \\\"first\\\": "
        "\\\"a\\\", \\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Unknown JSON escape\") and "
        "               not e.Message.Contains(\"failed to read\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Site 3: bz_binder_read_poly_discriminator, bz_jder_count_object_fields
// pre-count. Identical shape to #2427's own fixed instance: a truncated
// object (missing closing '}') before any "$type" field is read.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_truncated_before_type_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"leaf\\\", \\\"V\\\": 9\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_truncated_before_type_message_is_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"leaf\\\", \\\"V\\\": 9\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if not e.Message.Contains(\"missing\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Site 4: bz_binder_read_poly_discriminator, bz_jder_expect_char(':') after
// "$type" — by-inspection (byte-identical shape to LSan-confirmed site 1);
// this test's fail-without-fix / pass-with-fix run is this lane's own
// reachability confirmation.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_colon_missing_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\"X\\\"leaf\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_colon_missing_message_is_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\"X\\\"leaf\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Unexpected JSON token\") and "
        "               not e.Message.Contains(\"expected ':'\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Site 5: bz_binder_read_poly_discriminator, bz_jder_parse_string for the
// "$type" value — by-inspection (byte-identical shape to LSan-confirmed site
// 2); this test's fail-without-fix / pass-with-fix run is this lane's own
// reachability confirmation.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_value_bad_escape_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"\\\\z\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_value_bad_escape_message_is_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"\\\\z\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Unknown JSON escape\") and "
        "               not e.Message.Contains(\"failed to read\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// "Must still raise" controls: genuinely-empty objects (no pending
// exception, the ORIGINAL legitimate error path) must still raise their
// pre-existing messages for both discriminator kinds — the fix must not
// turn a required raise into a silent accept.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           case_genuinely_empty_object_still_raises_spec_9_8) {
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

BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_genuinely_empty_object_still_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var back := Base2.Deserialize(d); "
        "    return 1; "
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"missing\") and "
        "               e.Message.Contains(\"$type\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Clean round-trip control: the new pending-check is a no-op on the happy
// path (bz_exc is never set there) for the polymorphic type. Uses the
// hand-written "$type" JSON directly (matching #2419's own poly control),
// which sidesteps the unrelated fact that Leaf2.Serialize() called directly
// on the concrete type (rather than through a Base2-typed field) does not
// itself emit the "$type" header — that header is only emitted by a
// polymorphic-field container's own field-level serialize, exercised by the
// existing coverage in serialization_tier4_poly_test.cpp, not this fix. The
// "$case" choice's own clean-roundtrip controls are already covered by
// #2427's test file, not duplicated here.
// ===========================================================================
BEZEL_TEST(codegen_serialization_discriminator_double_throw_2432,
           poly_clean_roundtrip_unaffected_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kPoly +
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"leaf\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  using var back := Base2.Deserialize(d); "
        "  return (if back.Tag() = 7 then 42 else 1); "
        "end Main; end C;"), 42);
}
