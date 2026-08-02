// Codegen — issue #2419 / spec §9.8 "Choice serialization" (the `$case`
// discriminator) + §2.15 "Concrete serialization formats" (the JSON mapping
// and its `$`-reserved field set), docs revision v0.235.
//
// VERIFY-FIRST (scripts/agent_check.sh --probe --asan, against pre-fix
// origin/main): the issue's own repro —
//
//   choice Msg: ISerializable, IDeserializable
//     Both(first: string, second: int), Empty; end Msg;
//   ...
//   var json: string := "{\"$case\": \"Both\", \"first\": \"x\", \"second\": 5}";
//   using var d := new JsonDeserializer(json);
//   var v := Msg.Deserialize(d);   // raises here
//
// — reproduced exactly as filed: `Unhandled exception: SerializationException:
// Choice deserialize: failed to read "$case" value (§9.8)`, exit 70, on input
// that is ordinary, RFC 8259-legal, human/pretty-printed JSON (a single space
// after the "$case" colon). Removing that one space made the identical
// program succeed (exit 42, the program's own code), confirming the bug is
// specifically the missing whitespace skip between the "$case" colon and its
// value.
//
// Root cause (confirmed by reading bz_binder_read_case_discriminator's JSON
// branch in src/runtime/bezel_runtime_serialization.cpp): after
// `bz_jder_expect_char(d, ':')` consumes the colon (itself ws-tolerant on
// entry), the very next call was `bz_jder_parse_string(d)` with NO
// intervening `bz_jder_skip_ws(d)`. `bz_jder_parse_string_cps` requires
// `d->pos` to sit exactly on the opening '"' — any whitespace between the
// colon and the value is fatal there. Every ORDINARY field-value read in this
// file (e.g. `bz_binder_read_string`, used for regular record/class/choice-
// payload fields) already skips ws before parsing its value; only the two
// hand-written discriminator readers (`bz_binder_read_case_discriminator`'s
// "$case" and `bz_binder_read_poly_discriminator`'s "$type") were missing it.
// Fixed by inserting `bz_jder_skip_ws(d)` in both, immediately before their
// `bz_jder_parse_string(d)` call — mirroring the general-field-read pattern.
//
// Spec citations verified against the actual heading text (v0.235):
//   * spec §9.8 "Serialization synthesis" > "Choice serialization" subsection:
//     "The synthesized `Deserialize` reads the discriminator **first**,
//     consumed by case dispatch before any component binding — it is
//     structural, not a data field ... an undeclared discriminator string
//     raises `SerializationException`, as does an object that does not lead
//     with the discriminator."
//   * spec §9.8 "Polymorphic dispatch" subsection: the `$type` discriminator
//     is read "first, before any component of the object" — the structurally
//     identical sibling this fix also covers.
//   * stdlib §2.15 "Concrete serialization formats": defines the JSON mapping
//     and the `$`-reserved field set (`$id`, `$ref`, `$type`, `$case`).
//
// APIs verified against Bezel_Standard_Library.md: ISerializer/IDeserializer
// (§2.14), JsonSerializer/JsonDeserializer/SerializationException (§2.15).
//
// Issue #2419.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// The issue's own choice shape: a two-field payload case plus a payload-free
// case, so the discriminator dispatch and payload binding are both exercised.
const char* kMsg =
    "choice Msg: ISerializable, IDeserializable "
    "  Both(first: string, second: int), Empty; "
    "end Msg; ";

} // namespace

// ===========================================================================
// §9.8 "$case": whitespace-tolerance grid (the fix itself).
// ===========================================================================

// #2419 exact repro: a single space after the "$case" colon — ordinary
// human-written/pretty-printed JSON. Must deserialize the case AND its
// payload fields correctly (not merely "not raise").
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_space_after_colon_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"$case\\\": \\\"Both\\\", \\\"first\\\": \\\"x\\\", "
        "\\\"second\\\": 5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Both(var f, var s) => "
        "            (if f = \"x\" and s = 5 then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

// Whitespace on BOTH sides of the "$case" colon (`"$case" : "Both"` — the
// shape produced by symmetric key:value spacing). The leading side was
// already tolerated pre-fix (bz_jder_expect_char skips ws on entry); the
// trailing side needs this fix exactly like the after-colon-only case above.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_space_before_colon_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"$case\\\" : \\\"Both\\\", \\\"first\\\":\\\"x\\\", "
        "\\\"second\\\":5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Both(var f, var s) => "
        "            (if f = \"x\" and s = 5 then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

// A literal tab (not a space) between the "$case" colon and its value.
// bz_jder_skip_ws treats '\t' the same as ' ' (per its own definition), so
// this must round-trip identically to the space case.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_tab_after_colon_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"$case\\\":\\t\\\"Both\\\", \\\"first\\\":\\\"x\\\", "
        "\\\"second\\\":5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Both(var f, var s) => "
        "            (if f = \"x\" and s = 5 then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

// Newlines and indentation spacing around the colon and between fields —
// the shape `JsonOptions.Indented` output actually produces (stdlib §2.15:
// JsonSerializer writes without inter-token spaces for non-indented output,
// and WITH spacing under JsonOptions.Indented) — a realistic pretty-printed
// payload, not just a single injected space.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_newline_indented_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{\\n  \\\"$case\\\" :\\n    \\\"Both\\\",\\n"
        "  \\\"first\\\": \\\"x\\\",\\n  \\\"second\\\": 5\\n}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Both(var f, var s) => "
        "            (if f = \"x\" and s = 5 then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

// Payload-free case (`Empty`) with spacing around both the discriminator's
// colon and inside the object — total_count-1 == 0 remaining fields must
// still be handled correctly once the discriminator itself parses.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_payload_free_with_spacing_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{ \\\"$case\\\" : \\\"Empty\\\" }\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Empty => 42, Both(_, _) => 1); "
        "end Main; end C;"), 42);
}

// Unspaced control (the shape JsonSerializer itself always emits, and the
// only shape that worked pre-fix) — must still round-trip after the fix;
// bz_jder_skip_ws is a no-op when there is no whitespace to skip.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_unspaced_control_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"$case\\\":\\\"Both\\\",\\\"first\\\":\\\"x\\\","
        "\\\"second\\\":5}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Both(var f, var s) => "
        "            (if f = \"x\" and s = 5 then 42 else 1), "
        "          Empty => 2); "
        "end Main; end C;"), 42);
}

// Nested payload: a case whose payload is itself a serialization-eligible
// record, spaced heavily throughout (including inside the nested object) —
// confirms the fix composes with an ordinary nested-field read (which
// already tolerated whitespace) rather than only the flat/top-level shape.
BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_nested_record_payload_spacing_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int): ISerializable, IDeserializable end Point; "
        "choice Shape2: ISerializable, IDeserializable "
        "  Circle2(center: Point, radius: int), NoShape2; "
        "end Shape2; "
        "class C public static method Main(): int "
        "  var json: string := \"{ \\\"$case\\\" : \\\"Circle2\\\" , "
        "\\\"center\\\" : { \\\"X\\\" : 1 , \\\"Y\\\" : 2 } , "
        "\\\"radius\\\" : 5 }\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := Shape2.Deserialize(d); "
        "  return (case v of "
        "            Circle2(var c, var r) => "
        "              (if c.X = 1 and c.Y = 2 and r = 5 then 42 else 1), "
        "            NoShape2 => 2); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Negative control: whitespace tolerance must NOT relax the discriminator's
// leading-position requirement. §9.8: "an object that does not lead with the
// discriminator" raises `SerializationException` regardless of spacing.
// ===========================================================================

BEZEL_TEST(codegen_serialization_case_ws_2419,
           case_disc_not_leading_still_raises_with_spacing_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kMsg +
        "class C public static method Main(): int "
        "  var json: string := \"{ \\\"first\\\" : \\\"x\\\" , "
        "\\\"$case\\\" : \\\"Both\\\" , \\\"second\\\" : 5 }\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    var v := Msg.Deserialize(d); "
        "    return 1; "  // reached only if it wrongly did NOT raise
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"does not lead with\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 "$type" (polymorphic dispatch): the structurally identical sibling
// flagged (but not independently confirmed) by the issue.
// bz_binder_read_poly_discriminator's JSON branch had the exact same missing
// `bz_jder_skip_ws(d)` gap between its ':' and its value read; fixed
// alongside "$case" in the same commit.
// ===========================================================================

BEZEL_TEST(codegen_serialization_case_ws_2419,
           poly_type_disc_space_after_colon_json_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "abstract class Base2: ISerializable, IDeserializable "
        "  polymorphic(\"leaf\": Leaf2); "
        "  public abstract method Tag(): int; "
        "end Base2; "
        "class Leaf2 : Base2, ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Tag(): int return 7; end Tag; "
        "end Leaf2; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"$type\\\": \\\"leaf\\\", \\\"V\\\": 9}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  using var back := Base2.Deserialize(d); "
        "  return (if back.Tag() = 7 then 42 else 1); "
        "end Main; end C;"), 42);
}
